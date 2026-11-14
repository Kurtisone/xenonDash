// dash - « My Games » de la Games Library : jeux du registre vus comme jeux Arcade installés (phase P5b).
//
// Analyse du Dashboard 6770 (voir docs/technique.md) : la Games Library est une scène « Arcade:: » (2500_LiveArcadeHome.xur,
// Arcade::CMyArcadeMetaScene, Arcade::CTitleSelectorScene). Pour chaque contenu de type Arcade (0xD0000) énuméré,
// une tâche CLoaderTask (vtable 0x9201F8B8, thread de travail) :
//   1. XamContentGetAttributesInternal(contenu, attributs[36 octets], 0) doit réussir (dash_content.cpp) ;
//   2. XamContentCreateInternal(« racine: », contenu, 3 = ouvrir l'existant, ...) monte le paquet ;
//   3. lit « racine:\ArcadeInfo.xml » : élément <ArcadeInfo> > <TitleInfo Id Name Path ImagePath MaxCred ScoreName
//      CameraEnabled ContentNotDownloadable> (+ <Description>, <Locale> > <GameInfo>) ;
//   4. XamContentClose.
// Sur console, un jeu Arcade installé est un paquet STFS contenant ArcadeInfo.xml et l'image du jeu ; ici le paquet
// n'existe pas : XamContentCreateInternal est intercepté pour les contenus virtuels du registre (`library = true`),
// un répertoire contenant l'ArcadeInfo.xml généré est monté sous le nom de racine demandé, et XamContentClose le
// démonte. Les autres contenus passent au SDK.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <unistd.h>

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/types.h>

#include "dash_launch.h"

namespace dash::disc {
std::vector<uint8_t> IconFor(const launch::Game& g);
}

namespace rex::kernel::xam {
uint32_t XamContentCreateInternal_entry(mapped_string root_name, mapped_void content_data_ptr, uint32_t flags,
                                        mapped_u32 disposition_ptr, mapped_u32 license_mask_ptr,
                                        uint32_t cache_size, uint64_t content_size, mapped_void overlapped_ptr);
uint32_t XamContentClose_entry(mapped_string root_name, mapped_void overlapped_ptr);
}  // namespace rex::kernel::xam

namespace dash::library {

namespace fs = std::filesystem;

namespace {

std::mutex g_mutex;
std::set<std::string> g_roots;  // racines montées par nous

std::string Xml(const std::string& s) {
  std::string o;
  for (char c : s) {
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      default: o += c;
    }
  }
  return o;
}

fs::path PackageDir(const launch::Game& g) {
  auto dir = fs::temp_directory_path() / fmt::format("xenondash-content-{}", getuid()) / fmt::format("{:08X}", g.title_id);
  std::error_code ec;
  fs::create_directories(dir, ec);
  {
    // UTF-16 big-endian avec BOM (sub_92224fe0 saute un BOM 0xFEFF/0xFFFE puis lit des caractères larges).
    std::string utf8 = fmt::format(
        "<?xml version=\"1.0\" encoding=\"utf-16\"?>\n<ArcadeInfo>\n"
        "  <TitleInfo Id=\"{:08X}\" Name=\"{}\" Path=\"default.xex\" ImagePath=\"icon.png\" MaxCred=\"0\" "
        "ScoreName=\"\" CameraEnabled=\"false\" ContentNotDownloadable=\"true\">\n"
        "    <Description>{}</Description>\n  </TitleInfo>\n</ArcadeInfo>\n",
        g.title_id, Xml(g.name), Xml(g.name));
    std::string out = "\xFE\xFF";
    for (size_t i = 0; i < utf8.size();) {
      uint32_t c = uint8_t(utf8[i]);
      int n = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
      c = n == 0 ? c : (c & (0x3F >> n));
      for (int k = 0; k < n && i + 1 + k < utf8.size(); k++) c = (c << 6) | (uint8_t(utf8[i + 1 + k]) & 0x3F);
      i += 1 + n;
      auto put = [&](uint32_t u) { out += char(u >> 8), out += char(u & 0xFF); };
      if (c >= 0x10000) {
        c -= 0x10000;
        put(0xD800 + (c >> 10));
        put(0xDC00 + (c & 0x3FF));
      } else {
        put(c);
      }
    }
    std::ofstream x(dir / "ArcadeInfo.xml", std::ios::binary | std::ios::trunc);
    x.write(out.data(), std::streamsize(out.size()));
  }
  auto icon = dash::disc::IconFor(g);
  if (!icon.empty()) {
    std::ofstream i(dir / "icon.png", std::ios::binary | std::ios::trunc);
    i.write(reinterpret_cast<const char*>(icon.data()), std::streamsize(icon.size()));
  }
  return dir;
}

}  // namespace

// Nom de racine et contenu virtuel (TitleID + « %08X » en nom de fichier) -> montage ; sinon std::nullopt.
bool TryMount(const std::string& root, uint8_t* content, uint32_t flags, uint32_t* result) {
  if (!launch::LibraryEnabled()) return false;
  uint32_t title = __builtin_bswap32(*reinterpret_cast<uint32_t*>(content + 0x140));
  auto g = launch::Find(title);
  if (!g) return false;
  const char* fname = reinterpret_cast<const char*>(content + 8 + 0x100);  // nom de fichier : après le nom affiché
  (void)fname;
  auto dir = PackageDir(*g);
  auto* ks = rex::system::kernel_state();
  auto* vfs = ks->file_system();
  std::string device = fmt::format("\\Device\\DashGame_{:08X}", g->title_id);
  std::lock_guard lock(g_mutex);
  static std::set<uint32_t> registered;
  if (!registered.count(g->title_id)) {
    auto dev = std::make_unique<rex::filesystem::HostPathDevice>(device, dir, true);
    if (!dev->Initialize() || !vfs->RegisterDevice(std::move(dev))) {
      REXLOG_ERROR("[dash] bibliothèque : montage de {} impossible", dir.string());
      return false;
    }
    registered.insert(g->title_id);
  }
  vfs->RegisterSymbolicLink(root + ":", device);
  g_roots.insert(root);
  REXLOG_INFO("[dash] bibliothèque : contenu virtuel {:08X} « {} » monté sous {}: -> {} (indicateurs {:#x})",
              g->title_id, g->name, root, dir.string(), flags);
  *result = 0;
  return true;
}

bool TryUnmount(const std::string& root) {
  std::lock_guard lock(g_mutex);
  if (!g_roots.erase(root)) return false;
  rex::system::kernel_state()->file_system()->UnregisterSymbolicLink(root + ":");
  return true;
}

}  // namespace dash::library

// XamContentCreateInternal(racine, XCONTENT_AGGREGATE_DATA*, indicateurs, &disposition, &licences, cache, taille, overlapped)
REX_HOOK_RAW(__imp__XamContentCreateInternal) {
  uint32_t root_ptr = ctx.r3.u32, content = ctx.r4.u32, flags = ctx.r5.u32, disp = ctx.r6.u32, lic = ctx.r7.u32,
           ov = ctx.r10.u32;
  if (root_ptr && content) {
    std::string root(reinterpret_cast<const char*>(base + root_ptr));
    uint32_t r = 0;
    if (dash::library::TryMount(root, base + content, flags, &r)) {
      auto put = [&](uint32_t a, uint32_t v) {
        if (a) *reinterpret_cast<uint32_t*>(base + a) = __builtin_bswap32(v);
      };
      put(disp, 2);          // ouvert (existant)
      put(lic, 0xFFFFFFFFu);  // toutes les licences
      if (ov) {
        rex::system::kernel_state()->CompleteOverlappedImmediate(ov, r);
        r = 0x3E5;  // ERROR_IO_PENDING
      }
      ctx.r3.u64 = r;
      return;
    }
  }
  rex::ppc::HostToGuestFunction<rex::kernel::xam::XamContentCreateInternal_entry>(ctx, base);
}

REX_HOOK_RAW(__imp__XamContentClose) {
  if (ctx.r3.u32) {
    std::string root(reinterpret_cast<const char*>(base + ctx.r3.u32));
    if (dash::library::TryUnmount(root)) {
      uint32_t ov = ctx.r4.u32;
      if (ov) {
        rex::system::kernel_state()->CompleteOverlappedImmediate(ov, 0);
        ctx.r3.u64 = 0x3E5;
      } else {
        ctx.r3.u64 = 0;
      }
      return;
    }
  }
  rex::ppc::HostToGuestFunction<rex::kernel::xam::XamContentClose_entry>(ctx, base);
}

// XamContentLaunchImageInternal(XCONTENT_AGGREGATE_DATA*, indicateurs) : « Play Game » d'un jeu de « My Games »
// (sub_921A6128). Les jeux du registre sont lancés par le chemin habituel (superviseur + retour au Dashboard) ;
// ne revient pas si le lancement réussit. Sinon : erreur (« Game Not Found »).
REX_HOOK_RAW(__imp__XamContentLaunchImageInternal) {
  uint32_t content = ctx.r3.u32;
  REXLOG_INFO("[dash] XamContentLaunchImageInternal(contenu={:#x}, indicateurs={:#x})", content, ctx.r4.u32);
  if (content) {
    uint32_t title = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + content + 0x140));
    if (auto g = dash::launch::Find(title)) {
      dash::launch::LaunchGame(*g);
    }
  }
  ctx.r3.u64 = 0x80004005u;  // E_FAIL
}

// NXE/Metro : variante Ex (contenu, indicateurs, masque) de « Play Game » ; même traitement.
REX_HOOK_RAW(__imp__XamContentLaunchImageInternalEx) {
  uint32_t content = ctx.r3.u32;
  REXLOG_INFO("[dash] XamContentLaunchImageInternalEx(contenu={:#x}, indicateurs={:#x}, {:#x})", content, ctx.r4.u32,
              ctx.r5.u32);
  if (content) {
    uint32_t title = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + content + 0x140));
    if (auto g = dash::launch::Find(title)) {
      dash::launch::LaunchGame(*g);
    }
  }
  ctx.r3.u64 = 0x80004005u;  // E_FAIL
}
