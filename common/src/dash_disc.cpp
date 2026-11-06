// dash - disque simulé dans le lecteur (phase P5b).
//
// Un jeu du registre marqué `disc = true` (ou --dash_disc=<TitleID>) apparaît comme disque inséré. Ce que le
// Dashboard 6770 interroge pour son bouton de disque (relevé dans le code recompilé, voir README) :
//   1. notification système 0x8000000D, paramètre = type de média (1 = disque de jeu Xbox 360, 2 = Xbox
//      d'origine...) : sub_9210EB88 la dépile et sub_9210E478 range le type dans l'état global (0x92803A8C),
//      avec XamLoaderGetDvdTrayState. Sans cette notification l'état reste « aucun média » -> « Unplayable Disc ».
//      Elle n'est livrée qu'aux écouteurs existants : on la rediffuse après chaque création d'écouteur système ;
//   2. type 1 : un thread (sub_921B0DC8) ouvre \Device\Cdrom0\default.xex, vérifie l'en-tête XEX2 (hachage de
//      l'en-tête, résumé des pages, compression « basic »), retrouve la ressource nommée « %08X » du TitleID
//      (clés 0x40006 informations d'exécution, 0x2FF ressources, 0x3FF format de fichier) puis envoie
//      XMsgStartIORequest(0xFB, 0xB003C, {XDBF, taille, 40, tampon nom, &taille icône, tampon icône}) : le
//      message XAM renvoie le nom du titre (UTF-16, 40 caractères) et son icône PNG (16 384 octets au plus) ;
//   3. XamLoaderGetMediaInfo(&type, &TitleID) (voir dash_launch.cpp).
// Ici : un XEX synthétique (non chiffré, sans code) est généré dans un répertoire temporaire monté comme
// \Device\Cdrom0, l'application XAM 0xFB répond avec le nom et l'icône du registre, XexTransformImageKey
// (inutile sans chiffrement) renvoie 0, et la notification est rediffusée.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <unistd.h>

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/types.h>

#include "dash_gpd.h"
#include "dash_launch.h"

namespace rex::kernel::xam {
struct XMSGSTARTIOREQUEST_UNKNOWNARG;
uint32_t xeXMsgStartIORequestEx(uint32_t app, uint32_t message, uint32_t overlapped_ptr, uint32_t buffer_ptr,
                                uint32_t buffer_length, XMSGSTARTIOREQUEST_UNKNOWNARG* unknown);
uint32_t xeXamNotifyCreateListener(uint64_t mask, uint32_t is_system, uint32_t max_version);
}

REXCVAR_DECLARE(std::string, game_data_root);

namespace dash::disc {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kXnMediaChanged = 0x8000000D;  // paramètre : type de média
constexpr uint32_t kMediaGame360 = 1;

// ---- SHA-1 -------------------------------------------------------------------------------------------------
struct Sha1 {
  uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  uint8_t buf[64];
  size_t n = 0;
  uint64_t total = 0;
  static uint32_t rol(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }
  void block(const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = uint32_t(p[4 * i]) << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
      uint32_t f, k;
      if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
      else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
      else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
      else f = b ^ c ^ d, k = 0xCA62C1D6;
      uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d, d = c, c = rol(b, 30), b = a, a = t;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
  }
  void update(const uint8_t* p, size_t len) {
    total += len;
    while (len) {
      size_t k = std::min(len, 64 - n);
      std::memcpy(buf + n, p, k);
      n += k, p += k, len -= k;
      if (n == 64) block(buf), n = 0;
    }
  }
  std::vector<uint8_t> final() {
    uint64_t bits = total * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t z = 0;
    while (n != 56) update(&z, 1);
    uint8_t l[8];
    for (int i = 0; i < 8; i++) l[i] = uint8_t(bits >> (56 - 8 * i));
    update(l, 8);
    std::vector<uint8_t> out(20);
    for (int i = 0; i < 20; i++) out[i] = uint8_t(h[i / 4] >> (24 - 8 * (i % 4)));
    return out;
  }
};

std::vector<uint8_t> Sha1Of(std::initializer_list<std::pair<const uint8_t*, size_t>> parts) {
  Sha1 s;
  for (auto& [p, n] : parts) s.update(p, n);
  return s.final();
}

void PutBe32(std::vector<uint8_t>& v, size_t off, uint32_t x) {
  for (int i = 0; i < 4; i++) v[off + i] = uint8_t(x >> (24 - 8 * i));
}

// ---- XEX synthétique ---------------------------------------------------------------------------------------
// Disposition (voir l'analyse de sub_921B96A0 / sub_921B9900 / sub_921B9A90 / sub_921B95C0) :
//   0x000 en-tête 'XEX2', 0x1000 = début des données (pe), 0x200 = informations de sécurité, 3 en-têtes optionnels
//   0x040 format de fichier (non chiffré, compression « basic », 1 bloc de 0x10000 octets sans zéros)
//   0x080 ressources (1 : « %08X » du TitleID)    0x0C0 informations d'exécution (TitleID au décalage 12)
//   0x1000.. image de 0x10000 octets (1 page de 64 Ko) contenant la ressource.
std::vector<uint8_t> BuildXex(uint32_t title_id) {
  constexpr uint32_t kHeader = 0x1000, kImage = 0x10000, kSec = 0x200, kLoad = 0x80010000;
  std::vector<uint8_t> f(kHeader + kImage, 0);
  std::memcpy(f.data(), "XEX2", 4);
  PutBe32(f, 4, 0);
  PutBe32(f, 8, kHeader);
  PutBe32(f, 16, kSec);
  PutBe32(f, 20, 3);
  auto entry = [&](int i, uint32_t key, uint32_t off) {
    PutBe32(f, 24 + 8 * i, key);
    PutBe32(f, 28 + 8 * i, off);
  };
  entry(0, 0x000002FF, 0x80);
  entry(1, 0x000003FF, 0x40);
  entry(2, 0x00040006, 0xC0);
  // 0x3FF : taille, chiffrement (0 = aucun), compression (1 = basic), bloc (données, zéros)
  PutBe32(f, 0x40, 16);
  f[0x44] = 0, f[0x45] = 0, f[0x46] = 0, f[0x47] = 1;
  PutBe32(f, 0x48, kImage);
  PutBe32(f, 0x4C, 0);
  // 0x2FF : taille, puis entrées { nom[8], adresse, taille }
  PutBe32(f, 0x80, 4 + 16);
  std::string name = fmt::format("{:08X}", title_id);
  std::memcpy(&f[0x84], name.data(), 8);
  PutBe32(f, 0x8C, kLoad + 0x100);
  PutBe32(f, 0x90, 0x100);
  // 0x40006 : informations d'exécution (media_id, version, base_version, title_id, ...)
  PutBe32(f, 0xC0 + 12, title_id);
  // image : ressource « XDBF » minimale (le contenu est ignoré : le nom et l'icône viennent du registre)
  std::memcpy(&f[kHeader + 0x100], "XDBF", 4);
  // informations de sécurité
  PutBe32(f, kSec, kHeader);
  PutBe32(f, kSec + 4, kImage);
  PutBe32(f, kSec + 8 + 264, kLoad);  // adresse de chargement (secinfo + 0x110)
  // descripteur de page : 1 page, hachage du suivant (inutilisé)
  uint32_t desc = kSec + 388;
  PutBe32(f, kSec + 384, 1);
  PutBe32(f, desc, 1u << 4);
  // résumé : SHA-1(image || descripteur) à secinfo + 0x114
  auto digest = Sha1Of({{&f[kHeader], kImage}, {&f[desc], 24}});
  std::memcpy(&f[kSec + 0x114], digest.data(), 20);
  // hachage de l'en-tête (secinfo + 0x164) : SHA-1(f[sec+8+372 .. kHeader) || f[0 .. sec+8))
  size_t a = kSec + 8 + 372;
  auto hh = Sha1Of({{&f[a], kHeader - a}, {&f[0], kSec + 8}});
  std::memcpy(&f[kSec + 8 + 348], hh.data(), 20);
  return f;
}

fs::path g_disc_dir;
std::optional<launch::Game> g_game;

// Icône : `icon` (PNG) ou image 0x8000 d'un GPD/SPA (`spa`, sinon <TitleID>.gpd du profil local).
std::vector<uint8_t> LoadIcon(const launch::Game& g) {
  auto rel = [](const std::string& p) {
    fs::path x(p);
    return x.is_absolute() ? x : fs::absolute(REXCVAR_GET(game_data_root)) / x;
  };
  if (!g.icon.empty()) {
    std::ifstream f(rel(g.icon), std::ios::binary);
    if (f) return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  }
  std::vector<fs::path> cand;
  if (!g.spa.empty()) cand.push_back(rel(g.spa));
  for (auto& e : fs::directory_iterator(fs::absolute(REXCVAR_GET(game_data_root)) / "profiles", fs::directory_options::skip_permission_denied)) {
    if (e.is_directory()) cand.push_back(e.path() / fmt::format("{:08X}.gpd", g.title_id));
  }
  for (auto& c : cand) {
    gpd::File f;
    std::error_code ec;
    if (fs::exists(c, ec) && f.Load(c))
      if (auto img = f.Get(gpd::kImage, gpd::kTitleImageId)) return *img;
  }
  return {};
}

// ---- message XAM 0xFB / 0xB003C -------------------------------------------------------------------------
// L'application 0xFB est déjà prise par l'application XGI du SDK (qui journalise « Unimplemented XGI message »
// pour 0xB003C) : on intercepte XMsgStartIORequest(Ex) dans ce seul cas et on délègue le reste au SDK.
uint32_t HandleTitleInfo(uint8_t* base, uint32_t buffer_ptr, uint32_t buffer_length) {
  if (buffer_length < 24 || !g_game) return 0x80004005u;
  auto* m = base + buffer_ptr;
  auto rd = [&](int o) { return __builtin_bswap32(*reinterpret_cast<uint32_t*>(m + o)); };
  uint32_t name_cap = rd(8), name_ptr = rd(12), icon_size_ptr = rd(16), icon_ptr = rd(20);
  // Nom : UTF-8 -> UTF-16 big-endian.
  if (name_ptr && name_cap) {
    auto* w = base + name_ptr;
    std::u16string u;
    const std::string& s = g_game->name;
    for (size_t i = 0; i < s.size();) {
      uint32_t c = uint8_t(s[i]);
      int n = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
      c = n == 0 ? c : (c & (0x3F >> n));
      for (int k = 0; k < n && i + 1 + k < s.size(); k++) c = (c << 6) | (uint8_t(s[i + 1 + k]) & 0x3F);
      i += 1 + n;
      if (c >= 0x10000) {
        c -= 0x10000;
        u += char16_t(0xD800 + (c >> 10));
        u += char16_t(0xDC00 + (c & 0x3FF));
      } else {
        u += char16_t(c);
      }
    }
    if (u.size() > name_cap - 1) u.resize(name_cap - 1);
    for (size_t i = 0; i < u.size(); i++) w[2 * i] = uint8_t(u[i] >> 8), w[2 * i + 1] = uint8_t(u[i]);
    w[2 * u.size()] = w[2 * u.size() + 1] = 0;
  }
  // Icône PNG (le Dashboard réserve 16 384 octets).
  if (icon_size_ptr && icon_ptr) {
    auto* sz = reinterpret_cast<uint32_t*>(base + icon_size_ptr);
    uint32_t cap = __builtin_bswap32(*sz);
    auto icon = LoadIcon(*g_game);
    if (icon.empty() || icon.size() > cap) {
      if (!icon.empty()) REXLOG_WARN("[dash] icône du disque trop grande ({} > {} octets), ignorée", icon.size(), cap);
      icon.clear();
    }
    std::memcpy(base + icon_ptr, icon.data(), icon.size());
    *sz = __builtin_bswap32(uint32_t(icon.size()));
    REXLOG_INFO("[dash] disque {:08X} « {} » : nom et icône ({} octets) fournis au Dashboard", g_game->title_id,
                g_game->name, icon.size());
  }
  return 0;
}

std::atomic<uint64_t> g_generation{0};

}  // namespace

// Une seule diffusion par exécution : sur la console, la notification accompagne l'insertion du disque ; la
// rediffuser plus tard (écouteur créé par la Games Library...) serait prise pour une nouvelle insertion et le
// Dashboard lancerait le jeu tout seul (lancement automatique du disque inséré).
std::atomic<bool> g_sent{false};

void ScheduleMediaNotification() {
  if (!g_game || g_sent.load()) return;
  uint64_t gen = ++g_generation;
  std::thread([gen] {
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    if (gen != g_generation.load() || g_sent.exchange(true)) return;  // un écouteur plus récent reprogramme
    REXLOG_INFO("[dash] notification de média inséré (0x{:08X}, type {})", kXnMediaChanged, kMediaGame360);
    rex::system::kernel_state()->BroadcastNotification(rex::system::XNotificationID(kXnMediaChanged), kMediaGame360);
  }).detach();
}

bool Active() { return g_game.has_value(); }
std::vector<uint8_t> IconFor(const launch::Game& g) { return LoadIcon(g); }
uint32_t TitleInfo(uint8_t* base, uint32_t buffer_ptr, uint32_t buffer_length) {
  return HandleTitleInfo(base, buffer_ptr, buffer_length);
}

void Init() {
  g_game = launch::InsertedDisc();
  if (!g_game) return;
  g_disc_dir = fs::temp_directory_path() / fmt::format("xenondash-cdrom0-{}", getuid());
  std::error_code ec;
  fs::create_directories(g_disc_dir, ec);
  auto xex = BuildXex(g_game->title_id);
  {
    std::ofstream o(g_disc_dir / "default.xex", std::ios::binary | std::ios::trunc);
    o.write(reinterpret_cast<const char*>(xex.data()), std::streamsize(xex.size()));
  }
  auto* ks = rex::system::kernel_state();
  auto dev = std::make_unique<rex::filesystem::HostPathDevice>("\\Device\\Cdrom0", g_disc_dir, true);
  if (!dev->Initialize() || !ks->file_system()->RegisterDevice(std::move(dev))) {
    REXLOG_ERROR("[dash] disque simulé : montage de \\Device\\Cdrom0 impossible");
    g_game.reset();
    return;
  }
  REXLOG_INFO("[dash] disque simulé : {:08X} « {} » dans le lecteur (\\Device\\Cdrom0 -> {})", g_game->title_id,
              g_game->name, g_disc_dir.string());
}

}  // namespace dash::disc

// XexTransformImageKey : déchiffrement de la clé de fichier du XEX ; notre XEX n'est pas chiffré -> succès.
namespace dash::disc {
bool Active();
uint32_t TitleInfo(uint8_t* base, uint32_t buffer_ptr, uint32_t buffer_length);
}  // namespace dash::disc

REX_HOOK_RAW(__imp__XexTransformImageKey) {
  (void)base;
  ctx.r3.u64 = 0;
}

// Un écouteur système vient d'être créé : il n'a pas vu la notification de média -> la rediffuser.
REX_HOOK_RAW(__imp__XamNotifyCreateListenerInternal) {
  (void)base;
  uint64_t mask = ctx.r3.u64;
  ctx.r3.u64 = rex::kernel::xam::xeXamNotifyCreateListener(mask, ctx.r4.u32, ctx.r5.u32);
  if ((mask & 0x23) == 0x23) dash::disc::ScheduleMediaNotification();
}

REX_HOOK_RAW(__imp__XamNotifyCreateListener) {
  (void)base;
  uint64_t mask = ctx.r3.u64;
  ctx.r3.u64 = rex::kernel::xam::xeXamNotifyCreateListener(mask, 0, ctx.r4.u32);
  if ((mask & 0x23) == 0x23) dash::disc::ScheduleMediaNotification();
}

// XMsgStartIORequest(app, message, overlapped, buffer, longueur) et Ex (+ argument inconnu).
static void XMsgHook(PPCContext& ctx, uint8_t* base) {
  uint32_t app = ctx.r3.u32, msg = ctx.r4.u32, ov = ctx.r5.u32, buf = ctx.r6.u32, len = ctx.r7.u32;
  if (app == 0xFB && msg == 0xB003C && dash::disc::Active()) {
    uint32_t r = dash::disc::TitleInfo(base, buf, len);
    if (ov) {
      rex::system::kernel_state()->CompleteOverlappedImmediate(ov, r);
      r = 0x3E5;  // ERROR_IO_PENDING
    }
    ctx.r3.u64 = r;
    return;
  }
  ctx.r3.u64 = rex::kernel::xam::xeXMsgStartIORequestEx(
      app, msg, ov, buf, len, nullptr);
}
REX_HOOK_RAW(__imp__XMsgStartIORequest) { XMsgHook(ctx, base); }
REX_HOOK_RAW(__imp__XMsgStartIORequestEx) { XMsgHook(ctx, base); }
