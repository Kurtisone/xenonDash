// Test de common/src/host_timezone.h (sans dépendance) :
//   c++ -std=c++20 -Icommon/src common/tests/host_timezone_test.cpp -o /tmp/tz_test && /tmp/tz_test
// Affiche aussi le fuseau retenu pour l'hôte (essayer TZ=America/New_York, TZ=UTC...).
#include <cassert>
#include <cstdio>
#include <cstring>

#include "host_timezone.h"

using dash::tz::ParsePosix;
using dash::tz::Zone;

static void Show(const char* s) {
  Zone z;
  const bool ok = ParsePosix(s, z);
  std::printf("%-40s ok=%d biais=%d été=%d std=%.4s dlt=%.4s date_std=%02X%02X%02X%02X date_dlt=%02X%02X%02X%02X\n", s, ok, z.bias,
              z.dlt_bias, z.std_name, z.dlt_name, z.std_date.month, z.std_date.week, z.std_date.day_of_week, z.std_date.hour,
              z.dlt_date.month, z.dlt_date.week, z.dlt_date.day_of_week, z.dlt_date.hour);
}

int main() {
  Zone z;
  assert(ParsePosix("CET-1CEST,M3.5.0,M10.5.0/3", z));
  assert(z.bias == -60 && z.dlt_bias == -60 && std::memcmp(z.std_name, "CET", 4) == 0 && std::memcmp(z.dlt_name, "CEST", 4) == 0);
  assert(z.std_date.month == 10 && z.std_date.week == 5 && z.std_date.day_of_week == 0 && z.std_date.hour == 3);
  assert(z.dlt_date.month == 3 && z.dlt_date.week == 5 && z.dlt_date.day_of_week == 0 && z.dlt_date.hour == 2);
  assert(ParsePosix("EST5EDT,M3.2.0,M11.1.0", z) && z.bias == 300 && z.dlt_bias == -60 && z.dlt_date.week == 2);
  assert(ParsePosix("<+03>-3", z) && z.bias == -180 && z.dlt_date.month == 0 && std::memcmp(z.std_name, "+03\0", 4) == 0);
  assert(ParsePosix("IST-5:30", z) && z.bias == -330);
  assert(ParsePosix("UTC0", z) && z.bias == 0);
  assert(ParsePosix("NZST-12NZDT,M9.5.0,M4.1.0/3", z) && z.bias == -720 && z.dlt_date.month == 9);
  assert(ParsePosix("<-03>3<-02>,M3.5.0/-2,M10.5.0/-1", z) && z.dlt_date.hour == 0);
  assert(!ParsePosix("", z));
  assert(!ParsePosix("C", z));
  Show("CET-1CEST,M3.5.0,M10.5.0/3");
  Show("GMT0BST,M3.5.0/1,M10.5.0");
  Show("EST5EDT,M3.2.0,M11.1.0");
  Show("AEST-10AEDT,M10.1.0,M4.1.0/3");
  Show("<+0530>-5:30");
  const std::string host = dash::tz::HostPosixString();
  const Zone h = dash::tz::HostZone();
  std::printf("hôte : '%s' -> biais %d, été %d, source '%s'\n", host.c_str(), h.bias, h.dlt_bias, h.source.c_str());
  std::puts("tests OK");
  return 0;
}
