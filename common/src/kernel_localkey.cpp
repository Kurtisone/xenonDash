// xenondash common - signature LOCALE des paquets de contenu (XeKeysConsolePrivateKeySign / XeKeysConsoleSignatureVerification ;
// palier N4).
//
// Un paquet « CON » est signé par la clé PRIVÉE de la console : XAM signe l'en-tête à l'écriture (sub_8185A5B0) et le vérifie à la
// lecture (sub_8185A438, sinon ERROR_FILE_CORRUPT 0x80070570 : le profil créé n'était plus connectable après un redémarrage de
// l'interface, « Couldn't sign in your profile »). Le SDK renvoie 0 (échec pour ces fonctions qui rendent un BOOL).
// Aucune clé de console n'est fabriquée ni contournée : une clé SECRÈTE LOCALE de 32 octets, tirée au hasard au premier usage et
// rangée dans le dossier d'état de XAM (local_console.key), signe par HMAC-SHA-256. Une signature n'est valable que sur
// l'installation qui l'a produite ; un paquet venu d'une vraie console (signé par une autre clé) est refusé comme avant.

#include <sys/random.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "dash_sha256.h"
#include "dash_xam_runtime.h"

namespace {

constexpr size_t kHashSize = 20;      // SHA-1 de l'en-tête, calculé par XAM
constexpr size_t kSignatureSize = 0x100;
constexpr char kTag[8] = {'X', 'D', 'A', 'S', 'H', 'S', 'I', 'G'};

std::array<uint8_t, 32> LocalKey() {
  static std::mutex mu;
  static std::array<uint8_t, 32> key{};
  static bool ready = false;
  std::lock_guard lock(mu);
  if (ready) return key;
  const std::filesystem::path path = dash::xamrt::StateDir() / "local_console.key";
  std::ifstream in(path, std::ios::binary);
  if (in && in.read(reinterpret_cast<char*>(key.data()), key.size()) && size_t(in.gcount()) == key.size()) {
    ready = true;
    return key;
  }
  size_t got = 0;
  while (got < key.size()) {
    const ssize_t r = getrandom(key.data() + got, key.size() - got, 0);
    if (r <= 0) break;
    got += size_t(r);
  }
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(key.data()), key.size());
  REXLOG_INFO("[xekeys] clé locale de signature des paquets créée : {}", path.string());
  ready = true;
  return key;
}

std::array<uint8_t, 32> Mac(const uint8_t* hash) {
  const auto key = LocalKey();
  return dash::HmacSha256(key.data(), key.size(), hash, kHashSize);
}

}  // namespace

// XeKeysConsolePrivateKeySign(hachage SHA-1 (20 octets), signature (256 octets)) : 1 (BOOL) si signé.
REX_HOOK_RAW(__imp__XeKeysConsolePrivateKeySign) {
  const uint32_t hash = ctx.r3.u32, sig = ctx.r4.u32;
  if (!hash || !sig) {
    ctx.r3.u64 = 0;
    return;
  }
  const auto mac = Mac(base + hash);
  std::memset(base + sig, 0, kSignatureSize);
  std::memcpy(base + sig, mac.data(), mac.size());
  std::memcpy(base + sig + 32, kTag, sizeof kTag);
  ctx.r3.u64 = 1;
}

// XeKeysConsoleSignatureVerification(hachage, signature, &drapeau) : 1 si la signature vient de la clé locale ; *drapeau = 0
// (« signé par cette console »).
REX_HOOK_RAW(__imp__XeKeysConsoleSignatureVerification) {
  const uint32_t hash = ctx.r3.u32, sig = ctx.r4.u32, flag = ctx.r5.u32;
  if (!hash || !sig) {
    ctx.r3.u64 = 0;
    return;
  }
  const auto mac = Mac(base + hash);
  const bool ok = std::memcmp(base + sig, mac.data(), mac.size()) == 0 && std::memcmp(base + sig + 32, kTag, sizeof kTag) == 0;
  if (ok && flag) *reinterpret_cast<uint32_t*>(base + flag) = 0;
  ctx.r3.u64 = ok ? 1 : 0;
}
