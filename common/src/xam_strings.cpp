// XamLookupCommonStringByIndex(index) : texte d'un message de XAM, par index. Le SDK laisse cette
// fonction en REX_EXPORT_STUB (renvoie r3 inchangé) : XAM recevait donc l'index à la place d'un
// pointeur de chaîne, et ses boîtes (titre et texte) s'affichaient vides ou fausses.
//
// Source : <game_data_root>/flash/dashcomm.xzp, fichier dashStrings.xus (table XUIS du Dashboard,
// dans la langue livrée). Format relevé sur l'import 13604 : en-tête « XUIS », nombre d'entrées
// en big-endian à l'octet 10 (184), puis les chaînes ASCII terminées par un zéro à partir de l'octet 12.
// L'index est la position dans cette liste : 39 = « This is an installed game… » (disque),
// 104 = « Enter Pass Code », 59 = « Connect a camera… ».
//
// La valeur renvoyée est l'adresse invitée d'une chaîne UTF-16 big-endian (même convention que
// PutWide de dash_xam_redirect.cpp), mise en cache par index. Index inconnu : 0.
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include "kernel_sdk_fn.h"

REXCVAR_DECLARE(std::string, game_data_root);

namespace {

namespace fs = std::filesystem;

// Lit un fichier d'une archive XUIZ (v1 : noms UTF-16 big-endian, v3 : noms ASCII).
std::vector<uint8_t> ReadXzpEntry(const std::vector<uint8_t>& xzp, const std::string& wanted) {
  auto be32 = [&](size_t o) {
    return uint32_t(xzp[o]) << 24 | uint32_t(xzp[o + 1]) << 16 | uint32_t(xzp[o + 2]) << 8 | xzp[o + 3];
  };
  if (xzp.size() < 22 || std::memcmp(xzp.data(), "XUIZ", 4) != 0) return {};
  const uint32_t ver = be32(4), table_size = be32(16);
  const uint32_t count = uint32_t(xzp[20]) << 8 | xzp[21];
  size_t p = 22;
  const size_t data_start = 22 + size_t(table_size);
  for (uint32_t i = 0; i < count && p + 9 <= xzp.size(); ++i) {
    const uint32_t size = be32(p), off = be32(p + 4);
    const size_t nlen = xzp[p + 8];
    p += 9;
    std::string name;
    for (size_t k = 0; k < nlen && p + (ver >= 3 ? k : 2 * k + 1) < xzp.size(); ++k)
      name += ver >= 3 ? char(xzp[p + k]) : char(xzp[p + 2 * k + 1]);
    p += ver >= 3 ? nlen : nlen * 2;
    if (name == wanted && data_start + off + size <= xzp.size())
      return {xzp.begin() + data_start + off, xzp.begin() + data_start + off + size};
  }
  return {};
}

struct StringTable {
  bool loaded = false;
  std::vector<std::string> strings;  // position = index XAM
  std::mutex mu;
  std::unordered_map<uint32_t, uint32_t> guest;  // index -> adresse invitée
};

StringTable& Table() {
  static StringTable t;
  return t;
}

void LoadTable(StringTable& t) {
  if (t.loaded) return;
  t.loaded = true;
  const fs::path file = fs::absolute(REXCVAR_GET(game_data_root)) / "flash" / "dashcomm.xzp";
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    REXLOG_WARN("[strings] {} introuvable : textes XAM vides", file.string());
    return;
  }
  std::vector<uint8_t> xzp((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const auto xus = ReadXzpEntry(xzp, "dashStrings.xus");
  if (xus.size() < 12 || std::memcmp(xus.data(), "XUIS", 4) != 0) {
    REXLOG_WARN("[strings] dashStrings.xus absent de dashcomm.xzp : textes XAM vides");
    return;
  }
  const uint32_t count = uint32_t(xus[10]) << 8 | xus[11];
  size_t start = 12;
  for (size_t i = start; i <= xus.size() && t.strings.size() < count; ++i) {
    if (i == xus.size() || xus[i] == 0) {
      t.strings.emplace_back(reinterpret_cast<const char*>(xus.data() + start), i - start);
      start = i + 1;
    }
  }
  REXLOG_INFO("[strings] dashStrings.xus : {} chaînes", t.strings.size());
}

}  // namespace

REX_HOOK_RAW(__imp__XamLookupCommonStringByIndex) {
  (void)base;
  StringTable& t = Table();
  const uint32_t index = ctx.r3.u32;
  std::lock_guard<std::mutex> lock(t.mu);
  LoadTable(t);
  if (index >= t.strings.size()) {
    REXLOG_WARN("[strings] XamLookupCommonStringByIndex({}) : index inconnu", index);
    ctx.r3.u64 = 0;
    return;
  }
  auto found = t.guest.find(index);
  if (found == t.guest.end()) {
    const std::string& s = t.strings[index];
    const uint32_t n = uint32_t(s.size() + 1);
    const uint32_t a = REX_KERNEL_MEMORY()->SystemHeapAlloc(n * 2);
    auto* dst = reinterpret_cast<uint16_t*>(REX_KERNEL_MEMORY()->virtual_membase() + a);
    for (uint32_t i = 0; i < s.size(); ++i) dst[i] = __builtin_bswap16(uint16_t(uint8_t(s[i])));
    dst[s.size()] = 0;
    found = t.guest.emplace(index, a).first;
    REXLOG_INFO("[strings] XamLookupCommonStringByIndex({}) = \"{}\"", index, s);
  }
  ctx.r3.u64 = found->second;
}
