// Test de common/src/timezone_table.h (sans dépendance), sur des tables synthétiques des deux formats :
//   c++ -std=c++20 -O2 -Icommon/src common/tests/timezone_table_test.cpp -o /tmp/tzt_test && /tmp/tzt_test
// Avec des arguments, cherche aussi la table d'images réelles (image mémoire écrite par tools/xexinfo, hors du dépôt) et donne
// l'entrée retenue pour le fuseau de l'hôte :
//   /tmp/tzt_test <dash.img> [adresse de base, défaut 0x92000000] ...
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "timezone_table.h"

using dash::tz::Date;
using dash::tz::Table;
using dash::tz::TableFormat;
using dash::tz::Zone;

namespace {

constexpr uint32_t kLo = 0x1000, kHi = 0x80000;  // mémoire invitée synthétique [kLo, kHi)

struct Mem {
  std::vector<uint8_t> b = std::vector<uint8_t>(kHi, 0);
  void U8(uint32_t a, uint8_t v) { b[a] = v; }
  void U16(uint32_t a, uint16_t v) { b[a] = uint8_t(v >> 8), b[a + 1] = uint8_t(v); }
  void U32(uint32_t a, uint32_t v) { U16(a, uint16_t(v >> 16)), U16(a + 2, uint16_t(v)); }
  void W(uint32_t a, const char* s) {  // UTF-16BE terminé par un zéro
    for (; *s; ++s, a += 2) U16(a, uint8_t(*s));
    U16(a, 0);
  }
  // Données voisines d'une table, comme dans les images réelles : pointeurs vers l'image (biais apparents hors bornes).
  void Junk(uint32_t a, uint32_t n) {
    for (uint32_t i = 0; i < n; i += 4) U32(a + i, 0x0000A000u + i * 8);
  }
};

struct Rule {
  const char *std, *dlt;
  int16_t bias, dlt_bias;
  Date std_date, dlt_date;
};

const Date kNone{}, kEuStd{10, 5, 0, 3}, kEuDlt{3, 5, 0, 2}, kUkStd{10, 5, 0, 2}, kUkDlt{3, 5, 0, 1};

// Règles de la table synthétique : quelques fuseaux réels puis des entrées distinctes jusqu'à n.
std::vector<Rule> Rules(uint32_t n) {
  std::vector<Rule> r = {
      {"IDLW", "", 720, 0, kNone, kNone},
      {"GMT", "BST", 0, -60, kUkStd, kUkDlt},
      {"WEST", "WEDT", -60, -60, kEuStd, kEuDlt},  // Europe de l'Ouest : première entrée aux règles de Paris
      {"XYZ", "XYZD", -60, -60, kUkStd, kUkDlt},   // même biais et même nom que l'entrée suivante, autres dates
      {"XYZ", "XYZD", -60, -60, kEuStd, kEuDlt},   // le Dashboard retrouverait l'entrée précédente : écartée
      {"CET", "CEST", -60, -60, kEuStd, kEuDlt},   // règles de Paris et nom de l'hôte : retenue
  };
  static std::vector<std::string> names;
  names.resize(n);
  for (uint32_t i = uint32_t(r.size()); i < n; ++i) {
    names[i] = "Z" + std::to_string(i);
    r.push_back({names[i].c_str(), "", int16_t(-int(i)), 0, kNone, kNone});
  }
  return r;
}

void PutPointer32(Mem& m, uint32_t table, uint32_t strings, const std::vector<Rule>& rules) {
  uint32_t s = strings;
  auto str = [&](const char* x) {
    if (!*x) return 0u;
    const uint32_t a = s;
    m.W(a, x);
    s += 12;
    return a;
  };
  for (uint32_t i = 0; i < rules.size(); ++i) {
    const Rule& r = rules[i];
    const uint32_t e = table + 32 * i;
    const bool dst = r.dlt_bias != 0;
    m.U32(e, 600 + i);
    m.U16(e + 4, uint16_t(r.bias));
    m.U16(e + 6, 0);
    m.U16(e + 8, uint16_t(r.dlt_bias));
    m.U32(e + 0x0C, str(r.std));
    m.U8(e + 0x10, r.std_date.month), m.U8(e + 0x11, r.std_date.week), m.U8(e + 0x12, r.std_date.day_of_week),
        m.U8(e + 0x13, r.std_date.hour);
    m.U32(e + 0x14, dst ? str(r.dlt) : 0);
    m.U8(e + 0x18, r.dlt_date.month), m.U8(e + 0x19, r.dlt_date.week), m.U8(e + 0x1A, r.dlt_date.day_of_week),
        m.U8(e + 0x1B, r.dlt_date.hour);
    m.U32(e + 0x1C, dst ? 1 : 0);
  }
}

void PutInline244(Mem& m, uint32_t table, const std::vector<Rule>& rules) {
  auto systime = [&](uint32_t a, const Date& d) {
    m.U16(a, 0), m.U16(a + 2, d.month), m.U16(a + 4, d.day_of_week), m.U16(a + 6, d.week), m.U16(a + 8, d.hour);
  };
  for (uint32_t i = 0; i < rules.size(); ++i) {
    const Rule& r = rules[i];
    const uint32_t e = table + 244 * i;
    m.W(e, (std::string("CITY_") + r.std).c_str());
    m.U32(e + 0x40, 190 + i);
    m.U32(e + 0x44, uint32_t(int32_t(r.bias)));
    m.W(e + 0x48, r.std);
    systime(e + 0x88, r.std_date);
    m.U32(e + 0x98, 0);
    m.W(e + 0x9C, r.dlt);
    systime(e + 0xDC, r.dlt_date);
    m.U32(e + 0xEC, uint32_t(int32_t(r.dlt_bias)));
    m.U32(e + 0xF0, r.dlt_bias != 0 ? 1 : 0);
  }
}

Zone Paris() {
  Zone z;
  assert(dash::tz::ParsePosix("CET-1CEST,M3.5.0,M10.5.0/3", z));
  return z;
}

void TestPointer32() {
  Mem m;
  const auto rules = Rules(45);
  m.Junk(kLo, 0x1004);
  PutPointer32(m, kLo + 0x1004, kLo + 0x3000, rules);
  m.Junk(kLo + 0x1004 + 45 * 32, 0x40);
  const Table t = dash::tz::FindTable(m.b.data(), kLo, kHi);
  assert(t.format == TableFormat::kPointer32 && t.address == kLo + 0x1004 && t.count == 45);

  Zone z, out;
  assert(dash::tz::ReadEntry(m.b.data(), kLo, kHi, t, 1, z));
  assert(std::memcmp(z.std_name, "GMT", 4) == 0 && std::memcmp(z.dlt_name, "BST", 4) == 0 && z.dlt_bias == -60);
  assert(dash::tz::SameDate(z.std_date, kUkStd) && dash::tz::SameDate(z.dlt_date, kUkDlt));

  uint32_t index = 0;
  Zone host = Paris();
  assert(dash::tz::MatchZone(m.b.data(), kLo, kHi, t, host, out, index) && index == 5);  // CET : règles et nom
  std::memcpy(host.std_name, "XYZ", 4);
  assert(dash::tz::MatchZone(m.b.data(), kLo, kHi, t, host, out, index) && index == 2);  // XYZ (4) écartée : WEST
  host.bias = -45;
  assert(!dash::tz::MatchZone(m.b.data(), kLo, kHi, t, host, out, index));  // aucune entrée n'a ces règles
  std::printf("kPointer32 : table %05X, %u entrées, Paris -> entrée 5 (CET), XYZ -> entrée 2 (WEST)\n", t.address, t.count);
}

void TestInline244() {
  Mem m;
  const auto rules = Rules(50);
  m.Junk(kLo, 0x2000);
  PutInline244(m, kLo + 0x2000, rules);
  const Table t = dash::tz::FindTable(m.b.data(), kLo, kHi);
  assert(t.format == TableFormat::kInline244 && t.address == kLo + 0x2000 && t.count == 50);
  Zone z, out;
  assert(dash::tz::ReadEntry(m.b.data(), kLo, kHi, t, 2, z));
  assert(std::memcmp(z.std_name, "WEST", 4) == 0 && std::memcmp(z.dlt_name, "WEDT", 4) == 0 && z.bias == -60);
  assert(dash::tz::SameDate(z.std_date, kEuStd) && dash::tz::SameDate(z.dlt_date, kEuDlt));
  uint32_t index = 0;
  assert(dash::tz::MatchZone(m.b.data(), kLo, kHi, t, Paris(), out, index) && index == 5);
  std::printf("kInline244 : table %05X, %u entrées, Paris -> entrée 5 (CET)\n", t.address, t.count);
}

void TestShortRun() {
  Mem m;
  PutPointer32(m, kLo + 0x400, kLo + 0x3000, Rules(dash::tz::kMinTableEntries - 1));
  const Table t = dash::tz::FindTable(m.b.data(), kLo, kHi);
  assert(t.format == TableFormat::kNone && t.count == 0);
  std::printf("suite de %u entrées : ignorée\n", dash::tz::kMinTableEntries - 1);
}

// Image réelle (sortie de tools/xexinfo) : table trouvée, première et dernière entrées, entrée retenue pour l'hôte.
void Scan(const char* path, uint32_t lo) {
  std::ifstream f(path, std::ios::binary);
  const std::vector<uint8_t> img((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (img.empty()) {
    std::printf("%s : illisible\n", path);
    return;
  }
  const uint32_t hi = lo + uint32_t(img.size());
  const uint8_t* base = img.data() - lo;  // base[a] = octet de l'adresse invitée a, comme la mémoire invitée du SDK
  const Table t = dash::tz::FindTable(base, lo, hi);
  std::printf("%s : ", path);
  if (!t.count) {
    std::printf("aucune table\n");
    return;
  }
  Zone first, last, out;
  dash::tz::ReadEntry(base, lo, hi, t, 0, first);
  dash::tz::ReadEntry(base, lo, hi, t, t.count - 1, last);
  uint32_t index = 0;
  const Zone host = dash::tz::HostZone();
  const bool ok = dash::tz::MatchZone(base, lo, hi, t, host, out, index);
  std::printf("table %08X, %u %s, de %.4s à %.4s ; hôte « %s » -> ", t.address, t.count, dash::tz::FormatName(t.format),
              first.std_name, last.std_name, host.source.c_str());
  if (ok)
    std::printf("entrée %u (%.4s/%.4s)\n", index, out.std_name, out.dlt_name);
  else
    std::printf("aucune entrée\n");
}

}  // namespace

int main(int argc, char** argv) {
  TestPointer32();
  TestInline244();
  TestShortRun();
  for (int i = 1; i < argc; ++i) {
    uint32_t lo = 0x92000000u;
    if (i + 1 < argc && std::strncmp(argv[i + 1], "0x", 2) == 0) lo = uint32_t(std::strtoul(argv[i + 1], nullptr, 16));
    Scan(argv[i], lo);
    if (i + 1 < argc && std::strncmp(argv[i + 1], "0x", 2) == 0) ++i;
  }
  std::printf("OK\n");
  return 0;
}
