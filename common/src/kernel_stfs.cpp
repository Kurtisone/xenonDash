// xenondash common - pilote STFS minimal du noyau pour XAM recompilé (palier N4).
//
// Sur la console, XAM crée un paquet de contenu (en-tête « CON » écrit par XAM lui-même avec NtWriteFile), puis demande au
// pilote STFS du noyau d'en faire un périphérique (StfsCreateDevice) : le système de fichiers qui est DANS le paquet (arbre
// de hachage, table de fichiers, blocs) est tenu par le noyau, que XAM pilote par quelques codes (StfsControlDevice 0 à 5).
// Le SDK laisse ces deux fonctions en stub. Ici : le « volume » d'un paquet est un DOSSIER hôte, enregistré dans le système de
// fichiers virtuel sous le nom de périphérique que XAM a choisi ; XAM y crée ses fichiers (`Account`, ...) par NtCreateFile
// sur le nom de montage qu'il lie à ce périphérique (ObCreateSymbolicLink, kernel_ob.cpp).
//
// Dossier du volume :
//   Content\<XUID>\FFFE07D1\00010000\<XUID>  (paquet de profil) -> <game_data_root>/profiles/<XUID>/ (dossier du profil du Dashboard)
//   autre paquet                                                -> <game_data_root>/.stfs/<chemin relatif du paquet>/
// Ce n'est PAS un volume STFS : aucune structure STFS n'est écrite dans le paquet (seul l'en-tête l'est, par XAM), aucun
// hachage, aucune clé de console. Un paquet créé ici n'est lisible que par ce noyau natif.
//
// Codes de StfsControlDevice (déduits des appelants de XAM, sub_8185EF28 / sub_8185A5B0 / sub_8185A290) :
//   0 début d'écriture, 1 fin, 2 vérification, 3 lecture de 36 octets (empreinte du volume, comparée à l'en-tête),
//   4 validation (36 octets), 5 interrogation d'état. Tous réussissent ; le 3 renvoie l'empreinte du dossier du volume : quand le
//   contenu change, XAM réécrit l'en-tête et le signe avec XeKeysConsolePrivateKeySign (clé locale, kernel_localkey.cpp).

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "dash_sha256.h"

#include <rex/cvar.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/vfs.h>
#include <rex/hook.h>
#include <rex/runtime.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xfile.h>

namespace {

namespace fs = std::filesystem;

std::mutex g_mutex;
std::unordered_map<std::string, std::string> g_registered;  // nom de périphérique -> dossier du volume
std::unordered_map<uint32_t, std::string> g_devices;    // objet de périphérique invité -> nom

uint32_t Be32(const uint8_t* base, uint32_t ea) { return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + ea)); }
void PutBe32(uint8_t* base, uint32_t ea, uint32_t v) { *reinterpret_cast<uint32_t*>(base + ea) = __builtin_bswap32(v); }

// Dossier hôte du volume d'un paquet, d'après son chemin invité (« ...\Content\<XUID>\<titre>\<type>\<nom> »).
fs::path VolumeDir(const std::string& guest_path) {
  const fs::path root = fs::absolute(REXCVAR_GET(game_data_root));
  std::string rel = guest_path;
  const size_t c = rel.find("\\Content\\");
  rel = c == std::string::npos ? rel : rel.substr(c + 9);
  // <XUID>\<titre>\<type>\<nom>
  std::vector<std::string> part;
  size_t p = 0;
  while (p <= rel.size()) {
    size_t e = rel.find('\\', p);
    if (e == std::string::npos) e = rel.size();
    if (e > p) part.push_back(rel.substr(p, e - p));
    p = e + 1;
  }
  auto up = [](std::string s) {
    for (auto& ch : s) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    return s;
  };
  // Chemin de profil : [<XUID>\]FFFE07D1\00010000\<XUID du profil> (13604 : sans dossier de propriétaire).
  if (part.size() >= 3 && up(part[part.size() - 3]) == "FFFE07D1" && up(part[part.size() - 2]) == "00010000")
    return root / "profiles" / up(part.back());
  fs::path out = root / ".stfs";
  for (auto& s : part) out /= s;
  return out;
}

// Empreinte du volume (36 octets, comme le code 3 de la console : un descripteur de volume STFS) : SHA-256 des fichiers du dossier
// (chemin relatif, taille, contenu, par ordre alphabétique). Elle change quand le contenu change : XAM réécrit alors l'en-tête du
// paquet et le signe (kernel_localkey.cpp). Vide (zéros) pour un dossier vide.
std::array<uint8_t, 36> VolumeDigest(const fs::path& dir) {
  std::array<uint8_t, 36> out{};
  std::vector<fs::path> files;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (it->is_regular_file(ec)) files.push_back(it->path());
  }
  if (files.empty()) return out;
  std::sort(files.begin(), files.end());
  dash::Sha256 sha;
  for (const auto& f : files) {
    const std::string rel = fs::relative(f, dir, ec).generic_string();
    std::ifstream in(f, std::ios::binary);
    std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const uint64_t n = data.size();
    sha.Update(reinterpret_cast<const uint8_t*>(rel.data()), rel.size());
    sha.Update(reinterpret_cast<const uint8_t*>(&n), sizeof n);
    sha.Update(reinterpret_cast<const uint8_t*>(data.data()), data.size());
  }
  // Forme d'un descripteur de volume STFS (36 octets), que XAM range dans l'en-tête et revalide à la réouverture du paquet :
  // taille 0x24, séparation de blocs, nombre de blocs de la table de fichiers (BE16), numéro du premier bloc (3 octets), hachage de la
  // table de hachage racine (20 octets : ici le début du SHA-256 du contenu), blocs alloués (BE32), blocs libres (BE32).
  const auto d = sha.Final();
  const uint32_t allocated = uint32_t(files.size()) + 1;
  out[0] = 0x24;
  out[3] = 0;
  out[4] = 1;
  std::memcpy(out.data() + 8, d.data(), 20);
  for (int i = 0; i < 4; ++i) out[28 + i] = uint8_t(allocated >> (24 - 8 * i));
  return out;
}

}  // namespace

// StfsCreateDevice(params, 88) : params (88 octets, gros-boutiens) = { ANSI_STRING nom du périphérique ; +24 handle du fichier
// paquet ; +64 bloc de mémoire ; +68 taille de l'extension de périphérique demandée ; +72 SORTIE objet de périphérique ;
// +76 SORTIE extension de périphérique }.
REX_HOOK_RAW(__imp__StfsCreateDevice) {
  const uint32_t params = ctx.r3.u32;
  const uint16_t name_len = __builtin_bswap16(*reinterpret_cast<uint16_t*>(base + params));
  const uint32_t name_ptr = Be32(base, params + 4), handle = Be32(base, params + 24), ext_size = Be32(base, params + 68);
  const std::string name(reinterpret_cast<const char*>(base + name_ptr), name_len);

  std::string package;
  if (auto file = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XFile>(handle)) package = file->entry()->absolute_path();
  const fs::path dir = package.empty() ? fs::path() : VolumeDir(package);
  REXLOG_INFO("[stfs] StfsCreateDevice : périphérique '{}' (paquet '{}', extension {} octets) -> volume {}", name, package, ext_size,
              dir.string());
  if (name.empty() || dir.empty()) {
    ctx.r3.u64 = 0xC0000001u;  // STATUS_UNSUCCESSFUL
    return;
  }

  {
    std::lock_guard lock(g_mutex);
    // XAM nomme le périphérique d'après un identifiant de paquet qui peut être le même pour deux paquets (« Package_BEBE... ») :
    // un nom déjà enregistré pour un AUTRE dossier est remplacé (le volume précédent n'est plus monté).
    auto it = g_registered.find(name);
    if (it != g_registered.end() && it->second != dir.string()) {
      REX_KERNEL_FS()->UnregisterDevice(name);
      g_registered.erase(it);
    }
    if (!g_registered.count(name)) {
      std::error_code ec;
      fs::create_directories(dir, ec);
      auto dev = std::make_unique<rex::filesystem::HostPathDevice>(name, dir, /*read_only=*/false);
      if (!dev->Initialize() || !REX_KERNEL_FS()->RegisterDevice(std::move(dev))) {
        REXLOG_ERROR("[stfs] impossible de monter '{}' sur {}", name, dir.string());
        ctx.r3.u64 = 0xC0000001u;
        return;
      }
      g_registered[name] = dir.string();
    }
  }

  // Objet de périphérique (opaque pour XAM : il le garde, le référence et le repasse à StfsControlDevice) et extension de
  // périphérique (mémoire que XAM remplit : tout le contexte du paquet).
  const uint32_t device = REX_KERNEL_MEMORY()->SystemHeapAlloc(0x40);
  const uint32_t ext = REX_KERNEL_MEMORY()->SystemHeapAlloc(ext_size ? ext_size : 4);
  std::memset(base + device, 0, 0x40);
  std::memset(base + ext, 0, ext_size ? ext_size : 4);
  {
    std::lock_guard lock(g_mutex);
    g_devices[device] = name;
  }
  PutBe32(base, params + 72, device);
  PutBe32(base, params + 76, ext);
  ctx.r3.u64 = 0;
}

// StfsControlDevice(périphérique, code, tampon)
REX_HOOK_RAW(__imp__StfsControlDevice) {
  const uint32_t device = ctx.r3.u32, code = ctx.r4.u32, buffer = ctx.r5.u32;
  static std::atomic<int> n{0};
  if (n.fetch_add(1) < 12) REXLOG_INFO("[stfs] StfsControlDevice(0x{:08X}, {}, 0x{:08X})", device, code, buffer);
  if (code == 3 && buffer) {
    std::string dir;
    {
      std::lock_guard lock(g_mutex);
      auto it = g_devices.find(device);
      if (it != g_devices.end()) {
        auto reg = g_registered.find(it->second);
        if (reg != g_registered.end()) dir = reg->second;
      }
    }
    const auto h = dir.empty() ? std::array<uint8_t, 36>{} : VolumeDigest(dir);
    std::memcpy(base + buffer, h.data(), h.size());
  }
  ctx.r3.u64 = 0;
}
