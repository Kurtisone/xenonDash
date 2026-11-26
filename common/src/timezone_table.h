// xenondash common - table des fuseaux horaires d'un Dashboard, trouvée dans son image chargée (aucune adresse par version).
//
// Deux formats, gros-boutistes, relevés dans les images de 2.0.4532 à 2.0.17559 (une seule table par image) :
//   - kPointer32 (2.0.4532 à 2.0.13604 : 75 à 77 entrées, d'IDLW à KST) : entrées de 32 octets, +0x00 identifiant, +0x04 biais,
//     +0x06 biais d'hiver, +0x08 biais d'été (int16, minutes), +0x0C nom d'hiver (pointeur, UTF-16), +0x10 date d'hiver {mois,
//     semaine, jour, heure}, +0x14 nom d'été (pointeur, 0 sans heure d'été), +0x18 date d'été, +0x1C heure d'été (0/1).
//     Dans 13604 (0x929531A8, 77 entrées) : recherche du fuseau courant sub_9225C608 (biais puis nom ; à défaut, entrée n° 26 :
//     GMT, Londres), conversion sub_9225C468, vérification au démarrage sub_9225C7C0 (écrit l'entrée trouvée par
//     SetTimeZoneInformation, sub_927E08E0, si ses biais ou ses dates diffèrent des réglages).
//   - kInline244 (2.0.17559 : 83 entrées, de TOKELAU/IDLW à KIRIBATI/KST) : entrées de 244 octets, +0x00 clé (WCHAR[32],
//     « LONDON »), +0x40 identifiant, +0x44 TIME_ZONE_INFORMATION de Windows (biais int32, nom d'hiver WCHAR[32], date
//     SYSTEMTIME {année 0, mois, jour de semaine, semaine, heure...}, biais d'hiver, nom d'été WCHAR[32], date d'été, biais
//     d'été), +0xF0 heure d'été (0/1).
// Table retenue : la plus longue suite d'au moins kMinTableEntries entrées plausibles d'un même format. Aucune dépendance : le
// fichier se compile seul (test : common/tests/timezone_table_test.cpp).
#pragma once

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "host_timezone.h"

namespace dash::tz {

enum class TableFormat : uint8_t { kNone, kPointer32, kInline244 };

struct Table {
  uint32_t address = 0, count = 0;  // adresse invitée de la première entrée, nombre d'entrées
  TableFormat format = TableFormat::kNone;
};

constexpr uint32_t kMinTableEntries = 40;

inline uint32_t EntrySize(TableFormat f) {
  return f == TableFormat::kPointer32 ? 32 : f == TableFormat::kInline244 ? 244 : 0;
}

inline const char* FormatName(TableFormat f) {
  return f == TableFormat::kPointer32   ? "entrées de 32 octets, noms par pointeur"
         : f == TableFormat::kInline244 ? "entrées de 244 octets, TIME_ZONE_INFORMATION"
                                        : "aucun";
}

inline bool SameDate(const Date& a, const Date& b) {
  return a.month == b.month && a.week == b.week && a.day_of_week == b.day_of_week && a.hour == b.hour;
}

inline bool SameRules(const Zone& a, const Zone& b) {
  return a.bias == b.bias && a.std_bias == b.std_bias && a.dlt_bias == b.dlt_bias && SameDate(a.std_date, b.std_date) &&
         SameDate(a.dlt_date, b.dlt_date);
}

namespace detail {

// Mémoire invitée [lo, hi) (image du Dashboard) : octet de l'adresse invitée a = base[a].
struct Image {
  const uint8_t* base;
  uint32_t lo, hi;
  bool Has(uint32_t a, uint32_t n) const { return a >= lo && a <= hi && n <= hi - a; }
  uint16_t U16(uint32_t a) const { return uint16_t(uint16_t(base[a]) << 8 | base[a + 1]); }
  uint32_t U32(uint32_t a) const { return uint32_t(U16(a)) << 16 | U16(a + 2); }
};

// Nom UTF-16 de 1 à 4 caractères ASCII visibles (0 à 4 si empty_ok) terminé par un zéro, recopié dans out (complété de zéros).
inline bool Name4(const Image& m, uint32_t a, bool empty_ok, char (&out)[4]) {
  std::memset(out, 0, 4);
  if (!m.Has(a, 10)) return false;
  for (uint32_t i = 0; i < 5; ++i) {
    const uint16_t c = m.U16(a + 2 * i);
    if (!c) return i > 0 || empty_ok;
    if (i == 4 || c < 0x21 || c > 0x7E) return false;
    out[i] = char(c);
  }
  return false;
}

inline bool PlausibleBiases(int32_t bias, int32_t std_bias, int32_t dlt_bias) {
  return bias >= -14 * 60 && bias <= 12 * 60 && std_bias >= -120 && std_bias <= 120 && dlt_bias >= -120 && dlt_bias <= 120;
}

inline bool PlausibleDate(const Date& d) { return d.month <= 12 && d.week <= 5 && d.day_of_week <= 6 && d.hour <= 23; }

inline bool ReadPointer32(const Image& m, uint32_t e, Zone& z) {
  if (!m.Has(e, 32)) return false;
  const int32_t bias = int16_t(m.U16(e + 4)), std_bias = int16_t(m.U16(e + 6)), dlt_bias = int16_t(m.U16(e + 8));
  if (!PlausibleBiases(bias, std_bias, dlt_bias)) return false;
  const Date std_date{m.base[e + 0x10], m.base[e + 0x11], m.base[e + 0x12], m.base[e + 0x13]};
  const Date dlt_date{m.base[e + 0x18], m.base[e + 0x19], m.base[e + 0x1A], m.base[e + 0x1B]};
  if (!PlausibleDate(std_date) || !PlausibleDate(dlt_date)) return false;
  char std_name[4], dlt_name[4] = {};
  const uint32_t dlt_ptr = m.U32(e + 0x14);
  if (!Name4(m, m.U32(e + 0x0C), false, std_name) || (dlt_ptr && !Name4(m, dlt_ptr, true, dlt_name))) return false;
  z.bias = bias;
  z.std_bias = std_bias;
  z.dlt_bias = dlt_bias;
  std::memcpy(z.std_name, std_name, 4);
  std::memcpy(z.dlt_name, dlt_name, 4);
  z.std_date = std_date;
  z.dlt_date = dlt_date;
  return true;
}

// SYSTEMTIME {année, mois, jour de semaine, jour (= semaine du mois), heure, ...} d'une règle annuelle (année 0).
inline bool SystemTimeRule(const Image& m, uint32_t a, Date& d) {
  const uint16_t year = m.U16(a), month = m.U16(a + 2), dow = m.U16(a + 4), week = m.U16(a + 6), hour = m.U16(a + 8);
  if (year != 0 || month > 12 || dow > 6 || week > 5 || hour > 23) return false;
  d = {uint8_t(month), uint8_t(week), uint8_t(dow), uint8_t(hour)};
  return true;
}

inline bool ReadInline244(const Image& m, uint32_t e, Zone& z) {
  if (!m.Has(e, 244)) return false;
  const int32_t bias = int32_t(m.U32(e + 0x44)), std_bias = int32_t(m.U32(e + 0x98)), dlt_bias = int32_t(m.U32(e + 0xEC));
  if (!PlausibleBiases(bias, std_bias, dlt_bias) || m.U32(e + 0xF0) > 1) return false;
  for (uint32_t i = 0;; ++i) {  // clé : 1 à 31 caractères ASCII imprimables, terminée par un zéro
    if (i == 32) return false;
    const uint16_t c = m.U16(e + 2 * i);
    if (!c) {
      if (i == 0) return false;
      break;
    }
    if (c < 0x20 || c > 0x7E) return false;
  }
  Date std_date, dlt_date;
  char std_name[4], dlt_name[4];
  if (!SystemTimeRule(m, e + 0x88, std_date) || !SystemTimeRule(m, e + 0xDC, dlt_date) ||
      !Name4(m, e + 0x48, false, std_name) || !Name4(m, e + 0x9C, true, dlt_name))
    return false;
  z.bias = bias;
  z.std_bias = std_bias;
  z.dlt_bias = dlt_bias;
  std::memcpy(z.std_name, std_name, 4);
  std::memcpy(z.dlt_name, dlt_name, 4);
  z.std_date = std_date;
  z.dlt_date = dlt_date;
  return true;
}

inline bool ReadAt(const Image& m, TableFormat f, uint32_t e, Zone& z) {
  switch (f) {
    case TableFormat::kPointer32: return ReadPointer32(m, e, z);
    case TableFormat::kInline244: return ReadInline244(m, e, z);
    default: return false;
  }
}

}  // namespace detail

// Entrée i de la table t, lue dans la mémoire invitée (base[a] = octet de l'adresse invitée a). Tout ce qui est lu, noms compris,
// doit être dans [lo, hi) (image du Dashboard). Faux si l'entrée n'est pas plausible. Les champs de z autres que les règles et les
// noms (source) ne sont pas touchés.
inline bool ReadEntry(const uint8_t* base, uint32_t lo, uint32_t hi, const Table& t, uint32_t i, Zone& z) {
  const uint32_t size = EntrySize(t.format);
  if (!size || i >= t.count) return false;
  return detail::ReadAt({base, lo, hi}, t.format, t.address + i * size, z);
}

// Table des fuseaux dans [lo, hi) (lo multiple de 4) : la plus longue suite d'au moins kMinTableEntries entrées plausibles d'un
// même format, format kNone si aucune. Une passe par format et par décalage (multiple de 4) modulo la taille d'entrée : chaque
// position n'est lue qu'une fois par format.
inline Table FindTable(const uint8_t* base, uint32_t lo, uint32_t hi) {
  const detail::Image m{base, lo, hi};
  Table best;
  Zone z;
  for (TableFormat f : {TableFormat::kPointer32, TableFormat::kInline244}) {
    const uint32_t size = EntrySize(f);
    for (uint32_t phase = 0; phase < size; phase += 4) {
      uint32_t run = 0, start = 0;
      for (uint64_t a = uint64_t(lo) + phase; a + size <= hi; a += size) {
        if (detail::ReadAt(m, f, uint32_t(a), z)) {
          if (run++ == 0) start = uint32_t(a);
          continue;
        }
        if (run >= kMinTableEntries && run > best.count) best = {start, run, f};
        run = 0;
      }
      if (run >= kMinTableEntries && run > best.count) best = {start, run, f};
    }
  }
  return best;
}

// Entrée de la table pour le fuseau host : mêmes biais et mêmes dates ; parmi elles, celle qui porte le nom de l'hôte, sinon la
// première. Le Dashboard 13604 retrouve le fuseau courant par (biais, biais d'hiver, biais d'été) puis par nom (sub_9225C608) et
// le remplace si les dates de l'entrée trouvée diffèrent (sub_9225C7C0) : une entrée dont le nom désigne d'abord une autre entrée
// aux dates différentes est écartée. Faux si aucune entrée n'a ces règles ou si une entrée est illisible.
inline bool MatchZone(const uint8_t* base, uint32_t lo, uint32_t hi, const Table& t, const Zone& host, Zone& out,
                      uint32_t& index) {
  if (!t.count || t.count > 256) return false;
  std::vector<Zone> entries(t.count);
  for (uint32_t i = 0; i < t.count; ++i)
    if (!ReadEntry(base, lo, hi, t, i, entries[i])) return false;
  int best = -1;
  for (uint32_t i = 0; i < t.count; ++i) {
    const Zone& e = entries[i];
    if (!SameRules(e, host)) continue;
    uint32_t found = i;  // entrée que le Dashboard retrouverait pour ces biais et ce nom
    for (uint32_t j = 0; j < i; ++j) {
      const Zone& f = entries[j];
      if (f.bias == e.bias && f.std_bias == e.std_bias && f.dlt_bias == e.dlt_bias && std::memcmp(f.std_name, e.std_name, 4) == 0) {
        found = j;
        break;
      }
    }
    if (!SameRules(entries[found], e)) continue;
    if (best < 0) best = int(i);
    if (std::memcmp(e.std_name, host.std_name, 4) == 0) {
      best = int(i);
      break;
    }
  }
  if (best < 0) return false;
  out = entries[best];
  index = uint32_t(best);
  return true;
}

}  // namespace dash::tz
