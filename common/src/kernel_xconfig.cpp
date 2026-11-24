// xenondash common - réglages de console (XConfig) complémentaires pour XAM recompilé (palier N4).
//
// Le SDK n'implémente que quelques réglages (ExGetXConfigSetting : langue, fuseau, région AV...) et laisse ExSetXConfigSetting en
// stub. XAM recompilé lit et écrit d'autres réglages pendant la création d'un profil et la connexion : compteur de XUID
// (catégorie 1), adresse MAC (2), profil par défaut / contrôle parental (3), console (7)... Ici :
//   - ExSetXConfigSetting range la valeur en mémoire (jusqu'à la fin du processus) ;
//   - ExGetXConfigSetting rend d'abord la valeur rangée, puis celle du SDK, puis, pour un réglage de l'utilisateur (catégorie 3)
//     ou de la console (7) que le SDK ignore, des zéros de la taille demandée (console neuve : aucun réglage posé).
// Les catégories 1 (statistiques) et 2 (sécurisées : MAC, clés) restent en échec quand le SDK les ignore : XAM génère alors un XUID
// local aléatoire ; aucun identifiant ni secret de console n'est fabriqué.

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include <fmt/format.h>
#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "kernel_sdk_fn.h"

REXCVAR_DECLARE(std::string, game_data_root);

namespace {

std::mutex g_mutex;
std::map<uint32_t, std::vector<uint8_t>> g_values;  // (catégorie << 16 | réglage) -> octets (ordre de l'appelant)

uint32_t Key(uint32_t category, uint32_t setting) { return (category & 0xFFFF) << 16 | (setting & 0xFFFF); }

// Persistance : UNIQUEMENT 7/0x000C (drapeau de premier lancement), et seulement une valeur écrite par le RMW pendant l'exécution.
// Fichier : <game_data_root>/xconfig.toml, clé `console_000C` (entier). Absent, illisible ou mal formé : ignoré.
constexpr uint32_t kPersistedKey = (7u << 16) | 0x000Cu;

std::filesystem::path PersistPath() { return std::filesystem::absolute(REXCVAR_GET(game_data_root)) / "xconfig.toml"; }

// À appeler avec g_mutex tenu, avant toute lecture/écriture du magasin.
void LoadPersistedLocked() {
  static bool loaded = false;
  if (loaded) return;
  loaded = true;
  const auto path = PersistPath();
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) return;
  try {
    auto tbl = toml::parse_file(path.string());
    auto v = tbl["console_000C"].value<int64_t>();
    if (!v || *v < 0 || *v > 0xFFFFFFFFll) {
      REXLOG_WARN("[xconfig] {} : clé console_000C absente ou hors limites, fichier ignoré", path.string());
      return;
    }
    const uint32_t val = static_cast<uint32_t>(*v);
    auto& bytes = g_values[kPersistedKey];
    bytes = {uint8_t(val >> 24), uint8_t(val >> 16), uint8_t(val >> 8), uint8_t(val)};
    REXLOG_INFO("[xconfig] 7/0x000C = 0x{:08X} relu depuis {}", val, path.string());
  } catch (const std::exception& e) {
    REXLOG_WARN("[xconfig] {} illisible ou mal formé, ignoré : {}", path.string(), e.what());
  }
}

void PersistLocked(uint32_t val) {
  const auto path = PersistPath();
  auto tmp = path;
  tmp += ".tmp";
  std::error_code ec;
  {
    std::ofstream f(tmp, std::ios::trunc);
    f << "# Réglage XConfig 7/0x000C écrit par le Dashboard (ExReadModifyWriteXConfigSettingUlong).\n"
      << "console_000C = " << val << "\n";
    f.flush();
    if (!f) {
      REXLOG_WARN("[xconfig] écriture de {} impossible", tmp.string());
      std::filesystem::remove(tmp, ec);
      return;
    }
  }
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    REXLOG_WARN("[xconfig] renommage vers {} impossible : {}", path.string(), ec.message());
    std::filesystem::remove(tmp, ec);
    return;
  }
  REXLOG_INFO("[xconfig] 7/0x000C = {} persisté dans {}", val, path.string());
}

}  // namespace

// ExReadModifyWriteXConfigSettingUlong(catégorie, réglage, masque ET, valeur OU) : nouvelle = (ancienne & r5) | r6 (sémantique déduite).
REX_HOOK_RAW(__imp__ExReadModifyWriteXConfigSettingUlong) {
  const uint32_t category = ctx.r3.u32 & 0xFFFF, setting = ctx.r4.u32 & 0xFFFF, andMask = ctx.r5.u32, orMask = ctx.r6.u32;
  std::lock_guard lock(g_mutex);
  LoadPersistedLocked();
  auto& bytes = g_values[Key(category, setting)];
  uint32_t old = 0;
  if (bytes.size() == 4) old = uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 | uint32_t(bytes[2]) << 8 | bytes[3];
  const uint32_t val = (old & andMask) | orMask;
  bytes = {uint8_t(val >> 24), uint8_t(val >> 16), uint8_t(val >> 8), uint8_t(val)};
  REXLOG_INFO("[xconfig] RMW({}, 0x{:04X}) : 0x{:08X} -> 0x{:08X}", category, setting, old, val);
  if (Key(category, setting) == kPersistedKey) PersistLocked(val);
  ctx.r3.u64 = 0;
}

// ExSetXConfigSetting(catégorie, réglage, tampon, taille)
REX_HOOK_RAW(__imp__ExSetXConfigSetting) {
  const uint32_t category = ctx.r3.u32 & 0xFFFF, setting = ctx.r4.u32 & 0xFFFF, buf = ctx.r5.u32, size = ctx.r6.u32 & 0xFFFF;
  if (!buf || !size) {
    ctx.r3.u64 = 0xC000000Du;  // STATUS_INVALID_PARAMETER
    return;
  }
  {
    std::lock_guard lock(g_mutex);
    LoadPersistedLocked();
    g_values[Key(category, setting)].assign(base + buf, base + buf + size);
  }
  REXLOG_INFO("[xconfig] ExSetXConfigSetting({}, 0x{:04X}, {} octets)", category, setting, size);
  ctx.r3.u64 = 0;
}

// ExGetXConfigSetting(catégorie, réglage, tampon, tailleTampon, &tailleRequise)
REX_HOOK_RAW(__imp__ExGetXConfigSetting) {
  static PPCFunc* sdk = dash::SdkFunction("__imp__ExGetXConfigSetting");
  const uint32_t category = ctx.r3.u32 & 0xFFFF, setting = ctx.r4.u32 & 0xFFFF, buf = ctx.r5.u32, size = ctx.r6.u32 & 0xFFFF,
                 required = ctx.r7.u32;
  {
    std::lock_guard lock(g_mutex);
    LoadPersistedLocked();
    auto it = g_values.find(Key(category, setting));
    if (it != g_values.end()) {
      if (required) *reinterpret_cast<uint16_t*>(base + required) = __builtin_bswap16(static_cast<uint16_t>(it->second.size()));
      if (!buf || size < it->second.size()) {
        ctx.r3.u64 = 0xC0000023u;  // STATUS_BUFFER_TOO_SMALL
        return;
      }
      std::memcpy(base + buf, it->second.data(), it->second.size());
      ctx.r3.u64 = 0;
      return;
    }
  }
  sdk(ctx, base);
  static std::atomic<int> logged{0};
  const uint32_t status = ctx.r3.u32;
  if (status != 0 && (category == 3 || category == 7) && buf && size) {
    std::memset(base + buf, 0, size);
    if (required) *reinterpret_cast<uint16_t*>(base + required) = __builtin_bswap16(static_cast<uint16_t>(size));
    if (logged.fetch_add(1) < 20)
      REXLOG_INFO("[xconfig] ExGetXConfigSetting({}, 0x{:04X}, {} octets) : réglage absent, zéros", category, setting, size);
    ctx.r3.u64 = 0;
  } else if (status != 0 && logged.fetch_add(1) < 20) {
    REXLOG_INFO("[xconfig] ExGetXConfigSetting({}, 0x{:04X}, {} octets) -> 0x{:08X}", category, setting, size, status);
  }
}
