// xenondash common - fuseau horaire de l'hôte, au format des réglages XConfig de la console (catégorie 3, réglages 1 à 7).
//
// Source : la chaîne POSIX du fuseau (« CET-1CEST,M3.5.0,M10.5.0/3 »), lue dans TZ (chaîne POSIX, nom de zone « Europe/Paris »
// ou chemin) ou, à défaut, dans le pied du fichier TZif de /etc/localtime (versions 2 et suivantes). Aucune dépendance : le
// fichier se compile seul (test : common/tests/host_timezone_test.cpp).
//
// Format de la console, vérifié dans la table des fuseaux du Dashboard 13604 (0x929531A8, 77 entrées de 32 octets) : biais en
// minutes (UTC = local + biais ; Paris : -60), noms sur 4 caractères, dates de changement {mois, semaine du mois (5 = dernière),
// jour de semaine (0 = dimanche), heure} (Europe : heure d'hiver 0A050003, heure d'été 03050002), biais d'été -60.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace dash::tz {

// XCONFIG_TIMEZONE_DATE
struct Date {
  uint8_t month = 0, week = 0, day_of_week = 0, hour = 0;
};

struct Zone {
  int32_t bias = 0;      // minutes, UTC = local + biais
  int32_t std_bias = 0;  // ajouté au biais en heure d'hiver (toujours 0 ici)
  int32_t dlt_bias = 0;  // ajouté au biais en heure d'été (-60 en général)
  char std_name[4] = {}, dlt_name[4] = {};
  Date std_date, dlt_date;  // fin et début de l'heure d'été ; nuls sans heure d'été
  std::string source;       // chaîne POSIX utilisée (journal)
};

namespace detail {

inline bool Number(std::string_view& s, long& v) {
  size_t n = 0;
  v = 0;
  while (n < s.size() && s[n] >= '0' && s[n] <= '9' && n < 8) v = v * 10 + (s[n++] - '0');
  s.remove_prefix(n);
  return n > 0;
}

// Nom alphabétique (3 lettres ou plus) ou entre chevrons (<+03>).
inline bool Name(std::string_view& s, std::string& out) {
  out.clear();
  if (!s.empty() && s[0] == '<') {
    const size_t e = s.find('>');
    if (e == std::string_view::npos || e < 2) return false;
    out.assign(s.substr(1, e - 1));
    s.remove_prefix(e + 1);
    return true;
  }
  size_t n = 0;
  while (n < s.size() && ((s[n] >= 'A' && s[n] <= 'Z') || (s[n] >= 'a' && s[n] <= 'z'))) ++n;
  if (n < 3) return false;
  out.assign(s.substr(0, n));
  s.remove_prefix(n);
  return true;
}

// [+-]hh[:mm[:ss]] en secondes (POSIX : positif à l'ouest de Greenwich, même signe que le biais de la console).
inline bool Offset(std::string_view& s, long& secs) {
  long sign = 1, h = 0, m = 0, sec = 0;
  if (!s.empty() && (s[0] == '+' || s[0] == '-')) {
    if (s[0] == '-') sign = -1;
    s.remove_prefix(1);
  }
  if (!Number(s, h)) return false;
  if (!s.empty() && s[0] == ':') {
    s.remove_prefix(1);
    if (!Number(s, m)) return false;
    if (!s.empty() && s[0] == ':') {
      s.remove_prefix(1);
      if (!Number(s, sec)) return false;
    }
  }
  secs = sign * (h * 3600 + m * 60 + sec);
  return true;
}

// Mm.w.d[/heure] ; les formes Jn et n (jour de l'année) n'ont pas d'équivalent sur la console.
inline bool Rule(std::string_view& s, Date& d) {
  if (s.empty() || s[0] != 'M') return false;
  s.remove_prefix(1);
  long m = 0, w = 0, wd = 0, t = 2 * 3600;
  if (!Number(s, m) || s.empty() || s[0] != '.') return false;
  s.remove_prefix(1);
  if (!Number(s, w) || s.empty() || s[0] != '.') return false;
  s.remove_prefix(1);
  if (!Number(s, wd)) return false;
  if (!s.empty() && s[0] == '/') {
    s.remove_prefix(1);
    if (!Offset(s, t)) return false;
  }
  if (m < 1 || m > 12 || w < 1 || w > 5 || wd < 0 || wd > 6) return false;
  long hour = t / 3600;  // heure entière (les règles en vigueur tombent à l'heure) ; heures étendues POSIX ramenées à 0..23
  if (hour < 0) hour = 0;
  if (hour > 23) hour = 23;
  d.month = uint8_t(m);
  d.week = uint8_t(w);
  d.day_of_week = uint8_t(wd);
  d.hour = uint8_t(hour);
  return true;
}

inline void Copy4(const std::string& name, char (&out)[4]) {
  for (size_t i = 0; i < 4; ++i) out[i] = i < name.size() ? name[i] : '\0';
}

// Pied d'un fichier TZif (v2+) : « \n<chaîne POSIX>\n » à la fin du fichier.
inline std::string TzifFooter(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  const std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (data.size() < 46 || data.compare(0, 4, "TZif") != 0 || data[4] < '2' || data.back() != '\n') return {};
  const size_t start = data.rfind('\n', data.size() - 2);
  if (start == std::string::npos) return {};
  return data.substr(start + 1, data.size() - start - 2);
}

}  // namespace detail

// Analyse d'une chaîne POSIX. Faux si elle est mal formée ; une heure d'été sans règle représentable (forme Jn, règle absente)
// donne un fuseau sans heure d'été (biais d'hiver).
inline bool ParsePosix(std::string_view s, Zone& z) {
  z = Zone{};
  z.source.assign(s);
  std::string stdn, dltn;
  long std_off = 0;
  if (!detail::Name(s, stdn) || !detail::Offset(s, std_off)) return false;
  z.bias = int32_t(std_off / 60);
  detail::Copy4(stdn, z.std_name);
  if (s.empty()) return true;
  if (!detail::Name(s, dltn)) return false;
  long dlt_off = std_off - 3600;
  if (!s.empty() && s[0] != ',' && !detail::Offset(s, dlt_off)) return false;
  Date start, end;
  if (s.empty() || s[0] != ',') return true;
  s.remove_prefix(1);
  if (!detail::Rule(s, start) || s.empty() || s[0] != ',') return true;
  s.remove_prefix(1);
  if (!detail::Rule(s, end)) return true;
  detail::Copy4(dltn, z.dlt_name);
  z.dlt_bias = int32_t((dlt_off - std_off) / 60);
  z.dlt_date = start;
  z.std_date = end;
  return true;
}

// Chaîne POSIX du fuseau de l'hôte (vide si introuvable).
inline std::string HostPosixString() {
  if (const char* tz = std::getenv("TZ"); tz && *tz) {
    std::string v = tz;
    if (v[0] == ':') v.erase(0, 1);
    if (!v.empty() && v[0] == '/') return detail::TzifFooter(v);
    for (const char* dir : {"/usr/share/zoneinfo/", "/usr/lib/zoneinfo/", "/usr/share/lib/zoneinfo/"}) {
      std::string footer = detail::TzifFooter(std::string(dir) + v);
      if (!footer.empty()) return footer;
    }
    return v;  // chaîne POSIX telle quelle
  }
  return detail::TzifFooter("/etc/localtime");
}

// Fuseau de l'hôte. Contrôle : le décalage UTC actuel de l'hôte (localtime_r) doit être celui d'hiver ou celui d'été du fuseau
// analysé ; sinon (chaîne absente, illisible ou incohérente), biais fixe égal au décalage actuel, sans heure d'été.
inline Zone HostZone() {
  Zone z;
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  const int32_t current_bias = int32_t(-local.tm_gmtoff / 60);
  const std::string posix = HostPosixString();
  if (!posix.empty() && ParsePosix(posix, z) &&
      (z.bias + z.std_bias == current_bias || (z.dlt_date.month && z.bias + z.dlt_bias == current_bias)))
    return z;
  z = Zone{};
  z.bias = current_bias;
  z.source = posix.empty() ? "décalage actuel de l'hôte" : posix + " (incohérente : décalage actuel de l'hôte)";
  if (local.tm_zone) detail::Copy4(local.tm_zone, z.std_name);
  return z;
}

}  // namespace dash::tz
