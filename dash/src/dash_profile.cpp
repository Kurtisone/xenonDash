// dash - profils joueur locaux pour le Dashboard.
//
// Le runtime ReXGlue n'a qu'un utilisateur fixe ("User", toujours connecté) et laisse les
// fonctions de profil de XAM en stub. Le Dashboard, lui, gère les profils comme la console :
//   XamProfileCreateEnumerator / XamProfileEnumerate : liste des profils présents
//   XamProfileOpen(xuid, "DASHUSER") / XamProfileClose("DASHUSER") : montage du contenu du
//       profil (DASHUSER:\DashStyle, ThematicSkin, VisionEffect, DashPreferences...)
//   XamReadTileEx : image du joueur
//
// Ici : un profil local, celui de l'utilisateur 0 du runtime (même XUID), avec un gamertag
// réglable (--dash_gamertag). Son contenu est un dossier hôte inscriptible :
//   <game_data_root>/profiles/<XUID en hexadécimal>/
//
// Ces fonctions remplacent celles du SDK à l'édition de liens (le code généré les appelle
// directement) ; REX_HOOK et non REX_EXPORT pour ne pas doubler l'enregistrement par nom.

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xobject.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

REXCVAR_DEFINE_STRING(dash_gamertag, "Joueur", "Dash",
                      "Gamertag du profil local (15 caractères au plus)");

// Défini dans le SDK (xam_enum.cpp) sans en-tête public.
namespace rex::kernel::xam {
uint32_t xeXamEnumerate(uint32_t handle, uint32_t flags, rex::MappedPtr<void> buffer,
                        uint32_t buffer_length, uint32_t* items_returned, uint32_t overlapped);
}

namespace dash::profile {

using namespace rex;

// Structures console : alignement sur 4 octets.
#pragma pack(push, 4)

// X_XAMACCOUNTINFO (0x17C octets) : seuls le gamertag et les drapeaux sont renseignés.
struct XamAccountInfo {
  be<uint32_t> reserved_flags;          // 0x000
  be<uint32_t> live_flags;              // 0x004
  be<uint16_t> gamertag[16];            // 0x008 (UTF-16 big-endian)
  be<uint64_t> xuid_online;             // 0x028
  be<uint32_t> cached_user_flags;       // 0x030
  be<uint32_t> network_id;              // 0x034
  uint8_t passcode[4];                  // 0x038
  char online_domain[0x14];             // 0x03C
  char online_kerberos_realm[0x18];     // 0x050
  uint8_t online_key[0x10];             // 0x068
  char passport_membername[0x72];       // 0x078
  char passport_password[0x20];         // 0x0EA
  char owner_passport_membername[0x72]; // 0x10A
};
static_assert(sizeof(XamAccountInfo) == 0x17C);

// Élément renvoyé par XamProfileEnumerate (le Dashboard lit 8 + 0x17C octets, pile 0x188).
struct ProfileEnumResult {
  be<uint64_t> xuid_offline;
  XamAccountInfo account;
  be<uint32_t> device_id;
};
#pragma pack(pop)
static_assert(sizeof(ProfileEnumResult) == 0x188);

constexpr uint32_t kHddDeviceId = 1;

std::string Gamertag() {
  std::string g = REXCVAR_GET(dash_gamertag);
  if (g.empty()) g = "Joueur";
  if (g.size() > 15) g.resize(15);
  return g;
}

uint64_t LocalXuid() { return system::kernel_state()->user_profile()->xuid(); }

std::filesystem::path ProfileDir(uint64_t xuid) {
  return std::filesystem::absolute(REXCVAR_GET(game_data_root)) / "profiles" /
         fmt::format("{:016X}", xuid);
}

std::mutex g_mount_mutex;
std::set<std::string> g_mounted;  // noms de montage actifs ("DASHUSER")

// --- Utilisateur 0 : gamertag ------------------------------------------------

u32 XamUserGetName_entry(u32 user_index, mapped_string buffer, u32 buffer_len) {
  if (user_index >= 4) return X_E_INVALIDARG;
  if (user_index) return X_E_NO_SUCH_USER;
  auto name = Gamertag();
  uint32_t n = std::min<uint32_t>(buffer_len, 16);
  if (n == 0) return X_E_SUCCESS;
  std::memset(buffer, 0, n);
  std::memcpy(buffer, name.data(), std::min<size_t>(name.size(), n - 1));
  return X_E_SUCCESS;
}

u32 XamUserGetGamerTag_entry(u32 user_index, mapped_wstring buffer, u32 buffer_len) {
  if (user_index >= 4) return X_E_INVALIDARG;
  if (user_index) return X_E_NO_SUCH_USER;
  auto name = Gamertag();
  uint32_t n = std::min<uint32_t>(buffer_len, 16);
  auto* out = reinterpret_cast<be<uint16_t>*>(static_cast<char16_t*>(buffer));
  for (uint32_t i = 0; i < n; i++) {
    out[i] = i < name.size() ? uint16_t(uint8_t(name[i])) : 0;
  }
  if (n) out[n - 1] = 0;
  return X_E_SUCCESS;
}

// --- Énumération des profils -------------------------------------------------

u32 XamProfileCreateEnumerator_entry(u32 device_id, mapped_u32 handle_out) {
  if (!handle_out) return X_ERROR_INVALID_PARAMETER;
  auto e = system::make_object<system::XStaticUntypedEnumerator>(system::kernel_state(), 1,
                                                                 sizeof(ProfileEnumResult));
  auto result = e->Initialize(0xFF, 0xFE, 0x23001, 0x23003, 0);
  if (XFAILED(result)) return result;

  if (device_id == 0 || device_id == kHddDeviceId) {
    auto* item = reinterpret_cast<ProfileEnumResult*>(e->AppendItem());
    std::memset(item, 0, sizeof(*item));
    item->xuid_offline = LocalXuid();
    item->device_id = kHddDeviceId;
    auto name = Gamertag();
    for (size_t i = 0; i < name.size(); i++) item->account.gamertag[i] = uint8_t(name[i]);
  }
  *handle_out = e->handle();
  REXLOG_INFO("[dash] XamProfileCreateEnumerator(device={}) -> 1 profil '{}'", device_id, Gamertag());
  return X_ERROR_SUCCESS;
}

// Un profil par appel ; X_ERROR_NO_MORE_FILES quand la liste est épuisée.
u32 XamProfileEnumerate_entry(u32 handle, u32 flags, mapped_void buffer, u32 overlapped) {
  uint32_t returned = 0;
  return kernel::xam::xeXamEnumerate(handle, flags, buffer, sizeof(ProfileEnumResult),
                                     overlapped ? nullptr : &returned, overlapped);
}

// --- Montage du contenu du profil --------------------------------------------

u32 XamProfileOpen_entry(u64 xuid, mapped_string mount_name, u32 flags, u32 content_data) {
  (void)flags;
  (void)content_data;
  if (!mount_name) return X_ERROR_INVALID_PARAMETER;
  if (xuid != LocalXuid()) {
    REXLOG_WARN("[dash] XamProfileOpen({:016X}, {}) : profil inconnu (local = {:016X})",
                uint64_t(xuid), static_cast<const char*>(mount_name), LocalXuid());
    return X_ERROR_INVALID_PARAMETER;
  }

  std::string name(static_cast<const char*>(mount_name));
  std::lock_guard lock(g_mount_mutex);
  auto* vfs = system::kernel_state()->file_system();
  auto device_path = fmt::format("\\Device\\DashProfile_{}", name);
  if (!g_mounted.count(name)) {
    auto dir = ProfileDir(xuid);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    auto dev = std::make_unique<filesystem::HostPathDevice>(device_path, dir, /*read_only=*/false);
    if (!dev->Initialize() || !vfs->RegisterDevice(std::move(dev))) {
      REXLOG_ERROR("[dash] XamProfileOpen: impossible de monter {}", dir.string());
      return X_ERROR_FUNCTION_FAILED;
    }
    g_mounted.insert(name);
  }
  vfs->RegisterSymbolicLink(name + ":", device_path);
  REXLOG_INFO("[dash] XamProfileOpen({:016X}, {}) -> {}", uint64_t(xuid), name,
              ProfileDir(xuid).string());
  return X_ERROR_SUCCESS;
}

u32 XamProfileClose_entry(mapped_string mount_name) {
  if (!mount_name) return X_ERROR_INVALID_PARAMETER;
  std::string name(static_cast<const char*>(mount_name));
  // Le périphérique reste enregistré (réouverture fréquente) ; on retire seulement l'alias.
  system::kernel_state()->file_system()->UnregisterSymbolicLink(name + ":");
  return X_ERROR_SUCCESS;
}

// --- Image du joueur ---------------------------------------------------------

// Pas encore d'images : une vraie erreur fait afficher l'image par défaut (le stub du SDK
// laissait r3 = premier argument, pris pour un code de retour).
u32 XamReadTileEx_entry(u32 tile_type, u32 title_id, u64 tile_id, u32 user_index, u32 flag,
                        u32 buffer, u32 buffer_size_ptr, u32 overlapped) {
  (void)tile_type; (void)title_id; (void)tile_id; (void)user_index; (void)flag;
  (void)buffer; (void)buffer_size_ptr; (void)overlapped;
  return X_ERROR_FILE_NOT_FOUND;
}

}  // namespace dash::profile

REX_HOOK(__imp__XamUserGetName, dash::profile::XamUserGetName_entry)
REX_HOOK(__imp__XamUserGetGamerTag, dash::profile::XamUserGetGamerTag_entry)
REX_HOOK(__imp__XamProfileCreateEnumerator, dash::profile::XamProfileCreateEnumerator_entry)
REX_HOOK(__imp__XamProfileEnumerate, dash::profile::XamProfileEnumerate_entry)
REX_HOOK(__imp__XamProfileOpen, dash::profile::XamProfileOpen_entry)
REX_HOOK(__imp__XamProfileClose, dash::profile::XamProfileClose_entry)
REX_HOOK(__imp__XamReadTileEx, dash::profile::XamReadTileEx_entry)
