// dash - jeux joués, succès et images de jeu, lus dans les GPD du profil.
//
// Données : dash/assets/profiles/<XUID>/
//   FFFE07D1.gpd   liste des jeux joués (copiée d'un vrai profil, ou tools/make_test_gpd.py)
//   <TitleID>.gpd  succès et images de chaque jeu
//
// Appels du Dashboard 6770 (relevés dans le code recompilé) :
//   XamUserCreateTitlesPlayedEnumerator(0, utilisateur, xuid, départ, n, &taille, &handle)
//       puis XamEnumerate : 168 octets par jeu (0x28 de compteurs + nom UTF-16 sur 64 car.)
//   XamUserCreateAchievementEnumerator(titre, utilisateur, xuid, champs, départ, n, &taille, &handle)
//       puis XamEnumerate : X_ACHIEVEMENT_DETAILS (36 octets), chaînes après les structures
//   XamReadTile(type, titre, id, utilisateur, tampon, &taille, overlapped)

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xobject.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

#include "dash_gpd.h"

namespace dash::profile {
std::filesystem::path ProfileDir(uint64_t xuid);
uint64_t LocalXuid();
std::vector<uint8_t> LoadGamerPic(uint64_t xuid, bool small);
}  // namespace dash::profile

namespace dash::achievements {

using namespace rex;

constexpr uint32_t kDashTitleId = 0xFFFE07D1;

// --- Données du profil, chargées une fois ---------------------------------------

struct ProfileData {
  bool loaded = false;
  gpd::File dash;                                  // FFFE07D1.gpd
  std::map<uint32_t, std::unique_ptr<gpd::File>> titles;  // <TitleID>.gpd
};

std::mutex g_mutex;
ProfileData g_data;

ProfileData& Data() {
  if (!g_data.loaded) {
    g_data.loaded = true;
    auto dir = profile::ProfileDir(profile::LocalXuid());
    if (g_data.dash.Load(dir / "FFFE07D1.gpd")) {
      for (auto& t : g_data.dash.Titles()) {
        auto f = std::make_unique<gpd::File>();
        if (f->Load(dir / fmt::format("{:08X}.gpd", t.title_id))) g_data.titles[t.title_id] = std::move(f);
      }
      REXLOG_INFO("[dash] GPD : {} jeux joués, {} fichiers de jeu", g_data.dash.Titles().size(),
                  g_data.titles.size());
    }
  }
  return g_data;
}

const gpd::File* TitleGpd(uint32_t title_id) {
  auto& d = Data();
  auto it = d.titles.find(title_id);
  return it == d.titles.end() ? nullptr : it->second.get();
}

void PutW(be<uint16_t>* out, const std::u16string& s, size_t max_chars) {
  size_t n = std::min(s.size(), max_chars - 1);
  for (size_t i = 0; i < n; i++) out[i] = uint16_t(s[i]);
  for (size_t i = n; i < max_chars; i++) out[i] = 0;
}

// --- Jeux joués -------------------------------------------------------------------

#pragma pack(push, 4)
struct TitlePlayedItem {  // 168 octets
  be<uint32_t> title_id;
  be<uint32_t> achievements_possible;
  be<uint32_t> achievements_earned;
  be<uint32_t> gamerscore_total;
  be<uint32_t> gamerscore_earned;
  be<uint16_t> reserved;
  uint8_t avatar_awards[6];
  be<uint32_t> flags;
  be<uint32_t> last_played_hi;
  be<uint32_t> last_played_lo;
  be<uint16_t> name[64];
};
#pragma pack(pop)
static_assert(sizeof(TitlePlayedItem) == 168);

u32 XamUserCreateTitlesPlayedEnumerator_entry(u32 title_id, u32 user_index, u64 xuid, u32 offset,
                                              u32 count, mapped_u32 buffer_size, mapped_u32 handle) {
  (void)title_id; (void)xuid;
  if (!count || !buffer_size || !handle) return X_ERROR_INVALID_PARAMETER;
  if (user_index >= 4) return X_ERROR_INVALID_PARAMETER;
  std::lock_guard lock(g_mutex);
  auto titles = Data().dash.Titles();
  // Les plus récents d'abord, comme sur la console.
  std::sort(titles.begin(), titles.end(),
            [](auto& a, auto& b) { return a.last_played > b.last_played; });

  auto e = system::make_object<system::XStaticUntypedEnumerator>(system::kernel_state(), count,
                                                                 sizeof(TitlePlayedItem));
  auto result = e->Initialize(user_index, 0xFE, 0x23004, 0x23005, 0);
  if (XFAILED(result)) return result;
  for (size_t i = offset; i < titles.size(); i++) {
    const auto& t = titles[i];
    auto* item = reinterpret_cast<TitlePlayedItem*>(e->AppendItem());
    std::memset(item, 0, sizeof(*item));
    item->title_id = t.title_id;
    item->achievements_possible = t.achievements_possible;
    item->achievements_earned = t.achievements_earned;
    item->gamerscore_total = t.gamerscore_total;
    item->gamerscore_earned = t.gamerscore_earned;
    item->reserved = t.reserved;
    std::memcpy(item->avatar_awards, t.avatar, 6);
    item->flags = t.flags;
    item->last_played_hi = uint32_t(t.last_played >> 32);
    item->last_played_lo = uint32_t(t.last_played);
    PutW(item->name, t.name, 64);
  }
  *buffer_size = uint32_t(count * sizeof(TitlePlayedItem));
  *handle = e->handle();
  REXLOG_INFO("[dash] XamUserCreateTitlesPlayedEnumerator -> {} jeux", titles.size());
  return X_ERROR_SUCCESS;
}

// --- Succès -----------------------------------------------------------------------

struct AchievementDetails {  // X_ACHIEVEMENT_DETAILS, 36 octets
  be<uint32_t> id;
  be<uint32_t> label_ptr;
  be<uint32_t> description_ptr;
  be<uint32_t> unachieved_ptr;
  be<uint32_t> image_id;
  be<uint32_t> gamerscore;
  be<uint32_t> unlock_time_hi;
  be<uint32_t> unlock_time_lo;
  be<uint32_t> flags;
};
static_assert(sizeof(AchievementDetails) == 36);
constexpr size_t kStringBufferSize = 464;  // par succès, comme le SDK

class AchievementEnumerator : public system::XEnumerator {
 public:
  AchievementEnumerator(system::KernelState* ks, size_t per_enumerate, uint32_t fields,
                        std::vector<gpd::Achievement> items)
      : XEnumerator(ks, per_enumerate,
                    sizeof(AchievementDetails) + ((fields & 7) ? kStringBufferSize : 0)),
        fields_(fields), items_(std::move(items)) {}

  uint32_t WriteItems(uint32_t buffer_ptr, uint8_t* buffer_data, uint32_t* written) override {
    size_t count = std::min(items_.size() - next_, items_per_enumerate());
    if (!count) return X_ERROR_NO_MORE_FILES;
    auto* details = reinterpret_cast<AchievementDetails*>(buffer_data);
    size_t str_off = items_per_enumerate() * sizeof(AchievementDetails);
    uint32_t str_ptr = buffer_ptr + uint32_t(str_off);
    uint8_t* str_data = buffer_data + str_off;
    size_t remaining = count * kStringBufferSize;
    auto append = [&](const std::u16string& s) -> uint32_t {
      size_t bytes = (s.size() + 1) * 2;
      if (bytes > remaining) return 0;
      auto* w = reinterpret_cast<be<uint16_t>*>(str_data);
      for (size_t i = 0; i < s.size(); i++) w[i] = uint16_t(s[i]);
      w[s.size()] = 0;
      uint32_t p = str_ptr;
      str_ptr += uint32_t(bytes); str_data += bytes; remaining -= bytes;
      return p;
    };
    for (size_t i = 0; i < count; i++, next_++) {
      const auto& a = items_[next_];
      auto& d = details[i];
      d.id = a.id;
      d.label_ptr = (fields_ & 1) ? append(a.name) : 0;
      d.description_ptr = (fields_ & 2) ? append(a.unlocked_desc) : 0;
      d.unachieved_ptr = (fields_ & 4) ? append(a.locked_desc) : 0;
      d.image_id = a.image_id;
      d.gamerscore = uint32_t(a.gamerscore);
      d.unlock_time_hi = uint32_t(a.unlock_time >> 32);
      d.unlock_time_lo = uint32_t(a.unlock_time);
      d.flags = a.flags;
    }
    if (written) *written = uint32_t(count);
    return X_ERROR_SUCCESS;
  }

 private:
  uint32_t fields_;
  std::vector<gpd::Achievement> items_;
  size_t next_ = 0;
};

u32 XamUserCreateAchievementEnumerator_entry(u32 title_id, u32 user_index, u64 xuid, u32 fields,
                                             u32 offset, u32 count, mapped_u32 buffer_size,
                                             mapped_u32 handle) {
  (void)xuid;
  if (!count || !buffer_size || !handle || user_index >= 4) return X_ERROR_INVALID_PARAMETER;
  std::lock_guard lock(g_mutex);
  std::vector<gpd::Achievement> items;
  if (auto* f = TitleGpd(title_id)) {
    items = f->Achievements();
    std::sort(items.begin(), items.end(), [](auto& a, auto& b) { return a.id < b.id; });
    if (offset < items.size()) items.erase(items.begin(), items.begin() + offset);
    else items.clear();
  }
  size_t entry = sizeof(AchievementDetails) + ((fields & 7) ? kStringBufferSize : 0);
  *buffer_size = uint32_t(entry * count);
  auto e = system::object_ref<AchievementEnumerator>(
      new AchievementEnumerator(system::kernel_state(), count, fields, std::move(items)));
  auto result = e->Initialize(user_index, 0xFB, 0xB000A, 0xB000B, 0);
  if (XFAILED(result)) return result;
  *handle = e->handle();
  return X_ERROR_SUCCESS;
}

// --- Images : icônes de jeu et de succès --------------------------------------------

constexpr uint32_t kTileAchievement = 0, kTileGameIcon = 1, kTileGamer = 2, kTileGamerSmall = 3;

std::vector<uint8_t> FindTile(uint32_t type, uint32_t title_id, uint64_t tile_id) {
  if (type == kTileGamer || type == kTileGamerSmall) {
    return profile::LoadGamerPic(profile::LocalXuid(), type == kTileGamerSmall);
  }
  std::lock_guard lock(g_mutex);
  if (auto* f = TitleGpd(title_id)) {
    uint64_t id = type == kTileGameIcon ? gpd::kTitleImageId : tile_id;
    if (auto img = f->Get(gpd::kImage, id)) return *img;
  }
  if (type == kTileGameIcon) {  // icône aussi stockée dans le GPD du Dashboard (id = TitleID)
    if (auto img = Data().dash.Get(gpd::kImage, title_id)) return *img;
  }
  return {};
}

u32 XamReadTile_entry(u32 tile_type, u32 title_id, u64 tile_id, u32 user_index,
                      mapped_void buffer, mapped_u32 buffer_size, u32 overlapped) {
  (void)user_index;
  auto img = FindTile(tile_type, title_id, tile_id);
  X_RESULT result = X_ERROR_FILE_NOT_FOUND;
  uint32_t length = uint32_t(img.size());
  if (!img.empty() && buffer_size) {
    if (!buffer) {
      result = X_ERROR_SUCCESS;  // demande de taille seule
    } else if (*buffer_size < length) {
      result = 122;  // ERROR_INSUFFICIENT_BUFFER
    } else {
      std::memcpy(buffer, img.data(), length);
      result = X_ERROR_SUCCESS;
    }
    *buffer_size = length;
  }
  REXLOG_INFO("[dash] XamReadTile(type={}, titre={:08X}, id={:#x}, tampon={}) -> {:#x}, {} octets",
              uint32_t(tile_type), uint32_t(title_id), uint64_t(tile_id), buffer ? "oui" : "non",
              result, length);
  if (overlapped) {
    system::kernel_state()->CompleteOverlappedImmediateEx(overlapped, result, result, length);
    return X_ERROR_IO_PENDING;
  }
  return result;
}

// Titre système ? (Dashboard 0xFFFExxxx, applications système 0x58xxxxxx sauf les jeux Arcade
// 0x5841xxxx). Le stub du SDK renvoyait l'identifiant lui-même : tout jeu passait pour système.
u32 XamIsSystemTitleId_entry(u32 title_id) {
  if (title_id == 0) return 1;
  if ((title_id >> 16) == 0xFFFE) return 1;
  if ((title_id & 0xFF000000) == 0x58000000) return ((title_id >> 16) & 0xFF) != 0x41;
  return 0;
}

// Jeu Xbox originale ? Les titres Xbox 360 ont un numéro de jeu (16 bits bas) >= 0x7D0.
// Même piège que ci-dessus : le stub renvoyait l'identifiant, tout jeu passait pour Xbox 1
// (le Dashboard cherchait alors ses sauvegardes dans fusion:\UDATA).
u32 XamIsXbox1TitleId_entry(u32 title_id) {
  if (title_id == 0xFFFE0000) return 1;  // Dashboard Xbox 1
  if (title_id == 0 || (title_id >> 16) == 0xFFFE) return 0;
  return (title_id & 0xFFFF) < 0x7D0;
}

// Succès d'un jeu avec leurs images, pour l'écran de remplacement de XAM (dash_xam_ui.cpp).
struct TitleView {
  std::u16string name;
  std::vector<std::pair<gpd::Achievement, std::vector<uint8_t>>> items;
};

TitleView GetTitleView(uint32_t title_id) {
  std::lock_guard lock(g_mutex);
  TitleView view;
  for (auto& t : Data().dash.Titles()) {
    if (t.title_id == title_id) view.name = t.name;
  }
  if (auto* f = TitleGpd(title_id)) {
    auto list = f->Achievements();
    std::sort(list.begin(), list.end(), [](auto& a, auto& b) {
      if (a.earned() != b.earned()) return a.earned();          // obtenus d'abord,
      if (a.earned()) return a.unlock_time > b.unlock_time;     // les plus récents en tête
      return a.id < b.id;
    });
    for (auto& a : list) {
      auto img = f->Get(gpd::kImage, a.image_id);
      view.items.emplace_back(a, img ? *img : std::vector<uint8_t>{});
    }
  }
  return view;
}

// Gamerscore obtenu, d'après les GPD (0 sans données).
int32_t GpdGamerscore() {
  std::lock_guard lock(g_mutex);
  int32_t total = 0;
  for (auto& t : Data().dash.Titles()) total += int32_t(t.gamerscore_earned);
  return total;
}

}  // namespace dash::achievements

REX_HOOK(__imp__XamUserCreateTitlesPlayedEnumerator,
         dash::achievements::XamUserCreateTitlesPlayedEnumerator_entry)
REX_HOOK(__imp__XamUserCreateAchievementEnumerator,
         dash::achievements::XamUserCreateAchievementEnumerator_entry)
REX_HOOK(__imp__XamReadTile, dash::achievements::XamReadTile_entry)
REX_HOOK(__imp__XamIsSystemTitleId, dash::achievements::XamIsSystemTitleId_entry)
REX_HOOK(__imp__XamIsXbox1TitleId, dash::achievements::XamIsXbox1TitleId_entry)
