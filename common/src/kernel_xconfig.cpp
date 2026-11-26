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
#include <optional>
#include <vector>

#include <fmt/format.h>
#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xex_module.h>

#include "host_timezone.h"
#include "kernel_sdk_fn.h"
#include "timezone_table.h"

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

std::string_view Name4(const char (&name)[4]) { return {name, strnlen(name, 4)}; }

// Image chargée du Dashboard (module exécutable), en adresses invitées [lo, hi). Faux tant qu'elle n'est pas chargée.
bool DashboardImage(uint32_t& lo, uint32_t& hi) {
  rex::system::KernelState* ks = rex::system::kernel_state();
  if (!ks) return false;
  auto module = ks->GetExecutableModule();
  if (!module || !module->xex_module()) return false;
  const rex::runtime::XexModule* xex = module->xex_module();
  lo = xex->base_address();
  hi = lo + xex->image_size();
  return lo != 0 && hi > lo;
}

// Fuseau horaire rendu par les réglages 1 à 7 de la catégorie 3 : celui de l'hôte (host_timezone.h). Le SDK rend des zéros (UTC) :
// l'horloge du Guide affichait l'heure UTC (18:33 pour 19:33 à Paris, mesure du 2026-11-25). Les noms ne peuvent pas être ceux de
// l'hôte : au démarrage, le Dashboard 13604 cherche le fuseau courant dans sa table, par biais puis par nom, et à défaut écrit
// par-dessus son entrée par défaut (GMT, Londres : l'horloge retombait à l'heure UTC, mesure du 2026-11-25). D'où l'entrée de la
// table du Dashboard qui a les règles de l'hôte, table trouvée dans l'image chargée du Dashboard (timezone_table.h : 2.0.4532 à
// 2.0.17559, deux formats) ; sans table ou sans entrée de ces règles, des noms vides (le Dashboard ne vérifie pas un fuseau sans
// nom ; XAM n'utilise que les biais et les dates), de même pour le format de 17559 (voir plus bas). Le résultat n'est gardé
// qu'une fois l'image du Dashboard chargée.
dash::tz::Zone ConsoleZone(const uint8_t* base) {
  static std::mutex m;
  static std::optional<dash::tz::Zone> cached;
  static const dash::tz::Zone host = [] {
    const dash::tz::Zone h = dash::tz::HostZone();
    REXLOG_INFO("[xconfig] fuseau de l'hôte « {} » : biais {} min, heure d'été {} min, du {:02X}{:02X}{:02X}{:02X} au {:02X}{:02X}{:02X}{:02X}",
                h.source, h.bias, h.dlt_bias, h.dlt_date.month, h.dlt_date.week, h.dlt_date.day_of_week, h.dlt_date.hour,
                h.std_date.month, h.std_date.week, h.std_date.day_of_week, h.std_date.hour);
    return h;
  }();
  std::lock_guard lock(m);
  if (cached) return *cached;
  dash::tz::Zone z = host;
  std::memset(z.std_name, 0, 4);
  std::memset(z.dlt_name, 0, 4);
  uint32_t lo = 0, hi = 0;
  if (!DashboardImage(lo, hi)) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) REXLOG_INFO("[xconfig] fuseau lu avant le chargement du Dashboard : noms vides pour l'instant");
    return z;
  }
  const dash::tz::Table table = dash::tz::FindTable(base, lo, hi);
  if (!table.count) {
    REXLOG_INFO("[xconfig] aucune table des fuseaux dans l'image du Dashboard ({:08X}-{:08X}) : noms vides", lo, hi);
  } else {
    REXLOG_INFO("[xconfig] table des fuseaux du Dashboard : {:08X}, {} {}", table.address, table.count,
                dash::tz::FormatName(table.format));
    dash::tz::Zone entry;
    uint32_t index = 0;
    if (table.format == dash::tz::TableFormat::kInline244) {
      // 2.0.17559 (Metro) : noms vides, comme avant la recherche de la table. Avec les noms de l'entrée retenue, Metro plante
      // dès que la sélection passe sur Clock (Console Settings) : un de ses fils, lancé au démarrage (entrée 0x92392848), se
      // termine avec un X_KTHREAD entièrement à zéro et XThread::RundownAPCs lit une liste d'APC nulle (mesure du 2026-11-26 sous
      // gdb, `banc.py start --gdb` ; cause de la remise à zéro non trouvée, voir docs/REPRISE.md).
      REXLOG_INFO("[xconfig] fuseau de la console : noms vides pour ce format (plantage de Clock en 2.0.17559)");
    } else if (dash::tz::MatchZone(base, lo, hi, table, host, entry, index)) {
      REXLOG_INFO("[xconfig] fuseau de la console : entrée {} de la table du Dashboard ({}/{}), mêmes biais et dates", index,
                  Name4(entry.std_name), Name4(entry.dlt_name));
      entry.source = host.source;
      z = entry;
    } else {
      REXLOG_INFO("[xconfig] fuseau de la console : aucune entrée de la table du Dashboard n'a ces règles, noms vides");
    }
  }
  cached = z;
  return z;
}

// Réglage 1 à 7 de la catégorie 3 (4 octets, ordre de la console). Faux pour un autre réglage.
bool TimeZoneSetting(const uint8_t* base, uint32_t setting, uint8_t (&out)[4]) {
  const dash::tz::Zone zone = ConsoleZone(base);
  auto put32 = [&](int32_t v) {
    const uint32_t u = static_cast<uint32_t>(v);
    out[0] = uint8_t(u >> 24);
    out[1] = uint8_t(u >> 16);
    out[2] = uint8_t(u >> 8);
    out[3] = uint8_t(u);
  };
  auto put_date = [&](const dash::tz::Date& d) {
    out[0] = d.month;
    out[1] = d.week;
    out[2] = d.day_of_week;
    out[3] = d.hour;
  };
  switch (setting) {
    case 0x0001: put32(zone.bias); return true;                         // TIME_ZONE_BIAS
    case 0x0002: std::memcpy(out, zone.std_name, 4); return true;        // TIME_ZONE_STD_NAME
    case 0x0003: std::memcpy(out, zone.dlt_name, 4); return true;        // TIME_ZONE_DLT_NAME
    case 0x0004: put_date(zone.std_date); return true;                   // TIME_ZONE_STD_DATE
    case 0x0005: put_date(zone.dlt_date); return true;                   // TIME_ZONE_DLT_DATE
    case 0x0006: put32(zone.std_bias); return true;                      // TIME_ZONE_STD_BIAS
    case 0x0007: put32(zone.dlt_bias); return true;                      // TIME_ZONE_DLT_BIAS
    default: return false;
  }
}

// Valeur courante (4 octets) d'un réglage que le magasin ne contient pas : celle du SDK (ExGetXConfigSetting d'origine). Le tampon
// invité est pris sous le pointeur de pile de l'appelant (zone libre : la fonction du SDK est native et n'utilise pas la pile
// invitée) ; r3-r7 de l'appelant sont restaurés. Faux si le SDK ne connaît pas le réglage.
bool ReadSdkUlong(PPCContext& ctx, uint8_t* base, uint32_t category, uint32_t setting, uint32_t& out) {
  static PPCFunc* sdk = dash::SdkFunction("__imp__ExGetXConfigSetting");
  if (!sdk) return false;
  const uint64_t r3 = ctx.r3.u64, r4 = ctx.r4.u64, r5 = ctx.r5.u64, r6 = ctx.r6.u64, r7 = ctx.r7.u64;
  const uint32_t scratch = (ctx.r1.u32 - 0x200) & ~0xFu;
  std::memset(base + scratch, 0, 16);
  ctx.r3.u64 = category;
  ctx.r4.u64 = setting;
  ctx.r5.u64 = scratch;
  ctx.r6.u64 = 4;
  ctx.r7.u64 = scratch + 8;
  sdk(ctx, base);
  const bool ok = ctx.r3.u32 == 0;
  if (ok) out = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + scratch));
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  ctx.r6.u64 = r6;
  ctx.r7.u64 = r7;
  return ok;
}

}  // namespace

// ExReadModifyWriteXConfigSettingUlong(catégorie, réglage, masque ET, valeur OU) : nouvelle = (ancienne & r5) | r6 (sémantique déduite).
// L'ancienne valeur est celle que lirait ExGetXConfigSetting : la valeur rangée, sinon celle du SDK, sinon 0. Partir de 0 quand le
// SDK connaît le réglage effaçait ses bits : mesure du 2026-11-25, Getting Started fait RMW(3, 0x000C = RETAIL_FLAGS, |0x08000000) ;
// la valeur du SDK (0x40, « console configurée ») était perdue, le hub lisait ensuite 0x08000000 et lançait l'OOBE au lancement
// suivant (Video Library) : déconnexion de tous les profils (sub_9231E518 -> sub_92320920) et écran « manette, appuyez sur A ».
REX_HOOK_RAW(__imp__ExReadModifyWriteXConfigSettingUlong) {
  const uint32_t category = ctx.r3.u32 & 0xFFFF, setting = ctx.r4.u32 & 0xFFFF, andMask = ctx.r5.u32, orMask = ctx.r6.u32;
  std::lock_guard lock(g_mutex);
  LoadPersistedLocked();
  const bool stored = g_values.count(Key(category, setting)) != 0;
  uint32_t old = 0;
  if (!stored && ReadSdkUlong(ctx, base, category, setting, old))
    REXLOG_INFO("[xconfig] RMW({}, 0x{:04X}) : valeur de départ du SDK 0x{:08X}", category, setting, old);
  auto& bytes = g_values[Key(category, setting)];
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
  std::string hex;
  for (uint32_t i = 0; i < size && i < 8; ++i) hex += fmt::format("{:02X}", base[buf + i]);
  REXLOG_INFO("[xconfig] ExSetXConfigSetting({}, 0x{:04X}, {} octets) = {}{} (lr {:08X})", category, setting, size, hex,
              size > 8 ? "..." : "", static_cast<uint32_t>(ctx.lr));
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
  if (uint8_t tz[4]; category == 3 && TimeZoneSetting(base, setting, tz)) {
    if (required) *reinterpret_cast<uint16_t*>(base + required) = __builtin_bswap16(uint16_t(4));
    if (!buf || size < 4) {
      ctx.r3.u64 = 0xC0000023u;  // STATUS_BUFFER_TOO_SMALL
      return;
    }
    std::memcpy(base + buf, tz, 4);
    ctx.r3.u64 = 0;
    return;
  }
  // Réglages que le SDK ignore et que le Dashboard et XAM relisent en boucle : contrôle parental de l'utilisateur (3/0x000F,
  // PC_FLAGS, chaque seconde) et toute la catégorie console (7). Même réponse qu'après l'échec du SDK (zéros : aucun contrôle
  // parental, console neuve), mais sans son avertissement à chaque lecture (« Unimplemented XConfig ... »).
  if (((category == 3 && setting == 0x000F) || category == 7) && buf && size) {
    std::memset(base + buf, 0, size);
    if (required) *reinterpret_cast<uint16_t*>(base + required) = __builtin_bswap16(static_cast<uint16_t>(size));
    static std::atomic<int> quiet_logged{0};
    if (quiet_logged.fetch_add(1) < 8)
      REXLOG_INFO("[xconfig] ExGetXConfigSetting({}, 0x{:04X}, {} octets) : réglage absent, zéros", category, setting, size);
    ctx.r3.u64 = 0;
    return;
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
