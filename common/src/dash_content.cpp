// dash - contenu des jeux (sauvegardes, téléchargements, démos...) lu dans l'arbre du profil.
//
// Données : assets/profiles/<XUID>/Content/<TitleID>/<ContentType>/<fichier STFS>
// (et assets/profiles/0000000000000000/Content/... pour le contenu partagé), comme sur le disque dur
// de la console : Content/<XUID>/<TitleID>/<ContentType>/<paquet>. L'en-tête STFS (CON/LIVE/PIRS) donne
// le nom affiché, la description, la miniature, le type et le TitleID (tools/content_import.py).
//
// Appels du Dashboard 6770 (relevés dans le code recompilé) :
//   XamContentAggregateCreateEnumerator(xuid, appareil, type, titre, &handle)
//       r3 = XUID (0 = profil local), r4 = appareil (0 = tous), r5 = type de contenu (-1 = tous),
//       r6 = TitleID (0 = tous), r7 = &handle. Éléments XCONTENT_AGGREGATE_DATA (0x148 octets).
//   XamContentCreateEnumeratorInternal(utilisateur, appareil, type, titre, indicateurs, nombre,
//                                      &taille_tampon, &handle)   éléments XCONTENT_DATA (0x134 octets)
//   XamContentCreateEnumerator(utilisateur, appareil, type, indicateurs, nombre, &taille, &handle)

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

namespace dash::profile {
std::filesystem::path ProfileDir(uint64_t xuid);
uint64_t LocalXuid();
std::string Gamertag();
}  // namespace dash::profile

namespace dash::content {

using namespace rex;

constexpr uint32_t kDeviceHdd = 1;  // identifiant du disque dur factice du SDK (DummyDeviceId::HDD)
constexpr uint32_t kAnyType = 0xFFFFFFFF;

struct Item {
  uint64_t xuid = 0;  // 0 = contenu partagé
  uint32_t title_id = 0;
  uint32_t type = 0;
  std::string file_name;  // nom du paquet sur le disque
  std::u16string name;    // nom affiché
  std::u16string desc;
  std::vector<uint8_t> thumb;  // PNG
  uint64_t file_size = 0;
  std::filesystem::path path;  // vide pour le paquet de profil virtuel
};

constexpr uint32_t kTypeGamerProfile = 0x10000;
constexpr uint32_t kTitleDash = 0xFFFE07D1;

namespace {

uint32_t BE32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

std::u16string WStr(const uint8_t* p, size_t bytes) {
  std::u16string s;
  for (size_t i = 0; i + 1 < bytes; i += 2) {
    char16_t c = char16_t((p[i] << 8) | p[i + 1]);
    if (!c) break;
    s.push_back(c);
  }
  return s;
}

// Lit l'en-tête d'un paquet STFS ; faux si ce n'en est pas un.
bool ReadHeader(const std::filesystem::path& path, Item& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::vector<uint8_t> h(0x171A + 0x4000);
  f.read(reinterpret_cast<char*>(h.data()), h.size());
  size_t got = size_t(f.gcount());
  if (got < 0x171A) return false;
  if (std::memcmp(h.data(), "CON ", 4) && std::memcmp(h.data(), "LIVE", 4) && std::memcmp(h.data(), "PIRS", 4)) return false;
  out.type = BE32(&h[0x344]);
  out.title_id = BE32(&h[0x360]);
  // 18 langues de 0x80 octets : première non vide (l'anglais est en tête).
  for (int i = 0; i < 18 && out.name.empty(); i++) out.name = WStr(&h[0x411 + i * 0x80], 0x80);
  for (int i = 0; i < 18 && out.desc.empty(); i++) out.desc = WStr(&h[0xD11 + i * 0x80], 0x80);
  uint32_t tsize = BE32(&h[0x1712]);
  if (tsize && tsize <= 0x4000 && 0x171A + tsize <= got) out.thumb.assign(h.begin() + 0x171A, h.begin() + 0x171A + tsize);
  std::error_code ec;
  out.file_size = std::filesystem::file_size(path, ec);
  out.path = path;
  return true;
}

void ScanDir(uint64_t xuid, std::vector<Item>& out) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path root = profile::ProfileDir(xuid) / "Content";
  for (auto& t : fs::directory_iterator(root, ec)) {
    if (!t.is_directory(ec)) continue;
    uint32_t tid = uint32_t(std::strtoul(t.path().filename().string().c_str(), nullptr, 16));
    for (auto& ty : fs::directory_iterator(t.path(), ec)) {
      if (!ty.is_directory(ec)) continue;
      uint32_t type = uint32_t(std::strtoul(ty.path().filename().string().c_str(), nullptr, 16));
      for (auto& p : fs::directory_iterator(ty.path(), ec)) {
        if (!p.is_regular_file(ec)) continue;
        Item it;
        if (!ReadHeader(p.path(), it)) {
          REXLOG_WARN("[dash] contenu ignoré (pas un paquet STFS) : {}", p.path().string());
          continue;
        }
        // Le dossier fait foi (comme sur la console) ; l'en-tête sert de repli.
        it.xuid = xuid;
        it.title_id = tid;
        it.type = type;
        it.file_name = p.path().filename().string();
        if (it.name.empty()) it.name = std::u16string(it.file_name.begin(), it.file_name.end());
        out.push_back(std::move(it));
      }
    }
  }
}

}  // namespace

// Contenu filtré : titre 0 = tous, type kAnyType = tous. Profil local puis contenu partagé.
std::vector<Item> List(uint32_t title_id, uint32_t type) {
  std::vector<Item> all, out;
  uint64_t local = profile::LocalXuid();
  ScanDir(local, all);
  if (local != 0) ScanDir(0, all);
  if (local != 0) {
    // Paquet du profil (Content/<XUID>/FFFE07D1/00010000/<XUID> sur la console). Le Dashboard bâtit sa
    // liste de profils (nom du propriétaire d'un contenu) à partir de ces éléments, via
    // XamProfileLoadAccountInfo : sans lui, tout contenu a pour propriétaire « Unknown Profile ».
    Item p;
    p.xuid = local;
    p.title_id = kTitleDash;
    p.type = kTypeGamerProfile;
    p.file_name = fmt::format("{:016X}", local);
    auto g = profile::Gamertag();
    p.name = std::u16string(g.begin(), g.end());
    all.push_back(std::move(p));
  }
  for (auto& it : all) {
    if (title_id && it.title_id != title_id) continue;
    if (type != kAnyType && it.type != type) continue;
    out.push_back(std::move(it));
  }
  std::sort(out.begin(), out.end(), [](const Item& a, const Item& b) {
    if (a.title_id != b.title_id) return a.title_id < b.title_id;
    if (a.type != b.type) return a.type < b.type;
    return a.name < b.name;
  });
  return out;
}

namespace {

template <typename T>
void FillData(T* d, const Item& it) {
  std::memset(d, 0, sizeof(T));
  d->device_id = kDeviceHdd;
  d->content_type = system::XContentType(it.type);
  d->set_display_name(it.name);
  d->set_file_name(it.file_name);
}

// --- Énumération agrégée (tous les jeux ou un jeu) ---------------------------------

// Élément réel : XCONTENT_AGGREGATE_DATA (0x148 octets) suivi de la taille du paquet (BE64, +0x148),
// que le Dashboard affiche (« File Size ») ; il copie 512 octets par élément.
constexpr size_t kAggregateItemSize = 0x150;

class AggregateEnumerator : public system::XStaticUntypedEnumerator {
 public:
  AggregateEnumerator(system::KernelState* ks) : XStaticUntypedEnumerator(ks, 1, kAggregateItemSize) {}
};

u32 XamContentAggregateCreateEnumerator_entry(u64 xuid, u32 device_id, u32 content_type, u32 title_id,
                                              mapped_u32 handle_out) {
  if (!handle_out) return X_E_INVALIDARG;
  auto e = system::make_object<AggregateEnumerator>(system::kernel_state());
  system::X_KENUMERATOR_CONTENT_AGGREGATE* extra;
  auto result = e->Initialize(0xFF, 0xFE, 0x2000E, 0x20010, 0, &extra);
  if (XFAILED(result)) return result;
  extra->magic = system::kXObjSignature;
  extra->handle = e->handle();

  size_t n = 0;
  if (device_id == 0 || device_id == kDeviceHdd) {
    for (auto& it : List(title_id, content_type)) {
      if (xuid && it.xuid && it.xuid != xuid) continue;
      auto* raw = e->AppendItem();
      auto* d = reinterpret_cast<system::xam::XCONTENT_AGGREGATE_DATA*>(raw);
      FillData(d, it);
      d->xuid = it.xuid;
      d->title_id = it.title_id;
      *reinterpret_cast<be<uint64_t>*>(raw + 0x148) = it.file_size;
      n++;
    }
  }
  REXLOG_INFO("[dash] XamContentAggregateCreateEnumerator(xuid={:#x}, appareil={}, type={:#x}, titre={:08X}) -> {} éléments",
              uint64_t(xuid), uint32_t(device_id), uint32_t(content_type), uint32_t(title_id), n);
  *handle_out = e->handle();
  return X_ERROR_SUCCESS;
}

// --- Énumération simple (XCONTENT_DATA) -------------------------------------------

u32 CreateSimple(u32 device_id, u32 content_type, u32 title_id, u32 items_per_enumerate,
                 mapped_u32 buffer_size_ptr, mapped_u32 handle_out, const char* who) {
  if (!handle_out || !items_per_enumerate) return X_E_INVALIDARG;
  if (buffer_size_ptr) *buffer_size_ptr = uint32_t(sizeof(system::xam::XCONTENT_DATA) * items_per_enumerate);
  auto e = system::make_object<system::XStaticEnumerator<system::xam::XCONTENT_DATA>>(
      system::kernel_state(), items_per_enumerate);
  auto result = e->Initialize(0xFF, 0xFE, 0x20005, 0x20007, 0);
  if (XFAILED(result)) return result;
  size_t n = 0;
  if (device_id == 0 || device_id == kDeviceHdd) {
    for (auto& it : List(title_id, content_type)) {
      FillData(e->AppendItem(), it);
      n++;
    }
  }
  REXLOG_INFO("[dash] {}(appareil={}, type={:#x}, titre={:08X}, nombre={}) -> {} éléments", who,
              uint32_t(device_id), uint32_t(content_type), uint32_t(title_id), uint32_t(items_per_enumerate), n);
  *handle_out = e->handle();
  return X_ERROR_SUCCESS;
}

u32 XamContentCreateEnumeratorInternal_entry(u32 user_index, u32 device_id, u32 content_type, u32 title_id,
                                             u32 flags, u32 items_per_enumerate, mapped_u32 buffer_size_ptr,
                                             mapped_u32 handle_out) {
  (void)user_index; (void)flags;
  return CreateSimple(device_id, content_type, title_id, items_per_enumerate, buffer_size_ptr, handle_out,
                      "XamContentCreateEnumeratorInternal");
}

u32 XamContentCreateEnumerator_entry(u32 user_index, u32 device_id, u32 content_type, u32 flags,
                                     u32 items_per_enumerate, mapped_u32 buffer_size_ptr, mapped_u32 handle_out) {
  (void)user_index; (void)flags;
  return CreateSimple(device_id, content_type, 0, items_per_enumerate, buffer_size_ptr, handle_out,
                      "XamContentCreateEnumerator");
}

// --- Métadonnées d'un paquet --------------------------------------------------------
//
// XamContentGetMetaDataInternal(élément, tampon, 0) : élément = XCONTENT_AGGREGATE_DATA (0x148 octets,
//   celui de l'énumération), tampon de 0x93D6 octets = l'en-tête STFS de 0x344 à 0x971A (type de contenu,
//   TitleID à +0x1C, noms à +0xCD, descriptions à +0xFCD, miniature à +0x13D6...). Renvoie 0 ou une erreur.
// XamContentGetLocalizedString(métadonnées, nom, nb_car_nom, description, nb_car_description) : copie
//   le nom affiché et la description (UTF-16BE) dans les tampons non nuls.

constexpr uint32_t kMetaStart = 0x344, kMetaEnd = 0x971A, kMetaSize = kMetaEnd - kMetaStart;
static_assert(kMetaSize == 0x93D6);

u32 XamContentGetMetaDataInternal_entry(mapped_void item_ptr, mapped_void out, u32 flags) {
  (void)flags;
  if (!item_ptr || !out) return X_E_INVALIDARG;
  auto* d = static_cast<system::xam::XCONTENT_AGGREGATE_DATA*>(static_cast<void*>(item_ptr));
  std::string file = d->file_name();
  uint32_t type = uint32_t(system::XContentType(d->content_type));
  uint32_t title = uint32_t(d->title_id);
  for (auto& it : List(title, type)) {
    if (it.file_name != file) continue;
    auto* m = static_cast<uint8_t*>(static_cast<void*>(out));
    std::memset(m, 0, kMetaSize);
    if (it.path.empty()) {  // paquet de profil virtuel : en-tête minimal
      auto put32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; i++) m[o + i] = uint8_t(v >> (24 - 8 * i)); };
      put32(0, it.type);
      put32(0x360 - kMetaStart, it.title_id);
      for (size_t i = 0; i < it.name.size() && i < 0x3F; i++) {
        m[0x411 - kMetaStart + i * 2] = uint8_t(it.name[i] >> 8);
        m[0x411 - kMetaStart + i * 2 + 1] = uint8_t(it.name[i]);
      }
      return X_ERROR_SUCCESS;
    }
    std::ifstream f(it.path, std::ios::binary);
    f.seekg(kMetaStart);
    f.read(reinterpret_cast<char*>(m), kMetaSize);
    return X_ERROR_SUCCESS;
  }
  REXLOG_WARN("[dash] XamContentGetMetaDataInternal : '{}' (titre {:08X}, type {:#x}) introuvable", file, title, type);
  return X_ERROR_FILE_NOT_FOUND;
}

void CopyLocalized(const uint8_t* meta, size_t offset, be<uint16_t>* out, u32 chars) {
  if (!out || !chars) return;
  std::u16string s;
  for (int i = 0; i < 18 && s.empty(); i++) s = WStr(meta + offset + i * 0x80, 0x80);
  size_t n = std::min<size_t>(s.size(), chars - 1);
  for (size_t i = 0; i < n; i++) out[i] = uint16_t(s[i]);
  out[n] = 0;
}

u32 XamContentGetLocalizedString_entry(mapped_void meta_ptr, mapped_void name, u32 name_chars,
                                       mapped_void desc, u32 desc_chars) {
  if (!meta_ptr) return X_E_INVALIDARG;
  auto* meta = static_cast<const uint8_t*>(static_cast<void*>(meta_ptr));
  CopyLocalized(meta, 0x411 - kMetaStart, static_cast<be<uint16_t>*>(static_cast<void*>(name)), name_chars);
  CopyLocalized(meta, 0xD11 - kMetaStart, static_cast<be<uint16_t>*>(static_cast<void*>(desc)), desc_chars);
  return X_ERROR_SUCCESS;
}

// --- Suppression ------------------------------------------------------------------
// XamContentDeleteInternal(élément, overlapped) : supprime le paquet (ou renvoie une erreur).
u32 XamContentDeleteInternal_entry(mapped_void item_ptr, u32 overlapped) {
  X_RESULT result = X_ERROR_FILE_NOT_FOUND;
  if (item_ptr) {
    auto* d = static_cast<system::xam::XCONTENT_AGGREGATE_DATA*>(static_cast<void*>(item_ptr));
    std::string file = d->file_name();
    uint32_t type = uint32_t(system::XContentType(d->content_type));
    uint32_t title = uint32_t(d->title_id);
    for (auto& it : List(title, type)) {
      if (it.file_name != file) continue;
      if (it.path.empty()) { result = X_ERROR_ACCESS_DENIED; break; }  // profil virtuel
      std::error_code ec;
      std::filesystem::remove(it.path, ec);
      result = ec ? X_ERROR_ACCESS_DENIED : X_ERROR_SUCCESS;
      REXLOG_INFO("[dash] XamContentDeleteInternal : {} -> {:#x}", it.path.string(), uint32_t(result));
      break;
    }
  }
  if (overlapped) {
    system::kernel_state()->CompleteOverlappedImmediate(overlapped, result);
    return X_ERROR_IO_PENDING;
  }
  return result;
}

// --- Copie et déplacement -----------------------------------------------------------
// XamContentCopyInternal / XamContentMoveInternal(&nouveau_propriétaire ou 0, &appareil_destination ou 0,
//   élément, routine, contexte, 0, overlapped) : relevé dans l'explorateur de stockage. Le premier pointeur
//   n'est donné que si l'utilisateur n'est pas déjà propriétaire (« vous deviendrez le nouveau propriétaire »),
//   le second que si l'appareil change. Un seul disque est inscriptible : les autres appareils (lecteur
//   optique factice) refusent l'écriture ; sur le disque, seul un changement de propriétaire est possible
//   (le paquet passe d'un profil à l'autre ; DLC partagés = XUID 0).
u32 CopyOrMove(mapped_void owner_ptr, mapped_void device_ptr, mapped_void item_ptr, u32 overlapped, bool move) {
  X_RESULT result = X_ERROR_FUNCTION_FAILED;
  const char* what = move ? "Move" : "Copy";
  if (item_ptr) {
    auto* d = static_cast<system::xam::XCONTENT_AGGREGATE_DATA*>(static_cast<void*>(item_ptr));
    std::string file = d->file_name();
    uint32_t type = uint32_t(system::XContentType(d->content_type));
    uint32_t title = uint32_t(d->title_id);
    uint32_t dest_device = device_ptr ? uint32_t(*static_cast<be<uint32_t>*>(static_cast<void*>(device_ptr))) : 0;
    bool has_owner = bool(owner_ptr);
    uint64_t new_owner = has_owner ? uint64_t(*static_cast<be<uint64_t>*>(static_cast<void*>(owner_ptr))) : 0;
    for (auto& it : List(title, type)) {
      if (it.file_name != file || it.path.empty()) continue;
      if (device_ptr && dest_device != kDeviceHdd) {
        result = X_ERROR_ACCESS_DENIED;  // appareil en lecture seule
      } else if (!has_owner || new_owner == it.xuid) {
        result = X_ERROR_ALREADY_EXISTS;  // même appareil, même propriétaire : rien à faire
      } else {
        namespace fs = std::filesystem;
        fs::path dest_dir = profile::ProfileDir(new_owner) / "Content" / fmt::format("{:08X}", it.title_id) /
                            fmt::format("{:08X}", it.type);
        std::error_code ec;
        fs::create_directories(dest_dir, ec);
        fs::path dest = dest_dir / it.file_name;
        if (fs::exists(dest, ec)) {
          result = X_ERROR_ALREADY_EXISTS;
        } else {
          if (move) fs::rename(it.path, dest, ec);
          else fs::copy_file(it.path, dest, ec);
          result = ec ? X_ERROR_ACCESS_DENIED : X_ERROR_SUCCESS;
        }
      }
      REXLOG_INFO("[dash] XamContent{}Internal : {} (propriétaire {:#x} -> {}, appareil -> {}) -> {:#x}", what,
                  it.path.string(), it.xuid, has_owner ? fmt::format("{:#x}", new_owner) : "inchangé",
                  device_ptr ? std::to_string(dest_device) : "inchangé", uint32_t(result));
      break;
    }
  }
  if (overlapped) {
    system::kernel_state()->CompleteOverlappedImmediate(overlapped, result);
    return X_ERROR_IO_PENDING;
  }
  return result;
}

u32 XamContentCopyInternal_entry(mapped_void owner, mapped_void device, mapped_void item, u32 routine, u32 context,
                                 u32 unused, u32 overlapped) {
  (void)routine; (void)context; (void)unused;
  return CopyOrMove(owner, device, item, overlapped, false);
}

u32 XamContentMoveInternal_entry(mapped_void owner, mapped_void device, mapped_void item, u32 routine, u32 context,
                                 u32 unused, u32 overlapped) {
  (void)routine; (void)context; (void)unused;
  return CopyOrMove(owner, device, item, overlapped, true);
}

// --- Comparaison de chaînes ---------------------------------------------------------
// XamUniSortCmpString(a, b, nb_car_max, indicateurs) : compare deux chaînes UTF-16BE (au plus nb_car_max
// caractères) ; < 0, 0 ou > 0. Le Dashboard s'en sert pour trier et regrouper la liste Games de
// l'explorateur de stockage : il y cherche le jeu par son nom avec une recherche dichotomique. Le stub du
// SDK renvoyait r3 (jamais 0), donc aucun regroupement : une ligne par paquet. Indicateur 1 = sans casse.
int32_t XamUniSortCmpString_entry(mapped_void a, mapped_void b, u32 max_chars, u32 flags) {
  auto* pa = static_cast<const be<uint16_t>*>(static_cast<void*>(a));
  auto* pb = static_cast<const be<uint16_t>*>(static_cast<void*>(b));
  if (!pa || !pb) return pa == pb ? 0 : (pa ? 1 : -1);
  for (uint32_t i = 0; i < max_chars; i++) {
    uint32_t ca = pa[i], cb = pb[i];
    if (flags & 1) {
      if (ca < 0x80) ca = uint32_t(std::tolower(int(ca)));
      if (cb < 0x80) cb = uint32_t(std::tolower(int(cb)));
    }
    if (ca != cb) return ca < cb ? -1 : 1;
    if (!ca) return 0;
  }
  return 0;
}

}  // namespace
}  // namespace dash::content

REX_HOOK(__imp__XamContentAggregateCreateEnumerator, dash::content::XamContentAggregateCreateEnumerator_entry)
REX_HOOK(__imp__XamContentCreateEnumeratorInternal, dash::content::XamContentCreateEnumeratorInternal_entry)
REX_HOOK(__imp__XamContentGetMetaDataInternal, dash::content::XamContentGetMetaDataInternal_entry)
REX_HOOK(__imp__XamContentDeleteInternal, dash::content::XamContentDeleteInternal_entry)
REX_HOOK(__imp__XamContentGetLocalizedString, dash::content::XamContentGetLocalizedString_entry)
REX_HOOK(__imp__XamContentCopyInternal, dash::content::XamContentCopyInternal_entry)
REX_HOOK(__imp__XamContentMoveInternal, dash::content::XamContentMoveInternal_entry)
REX_HOOK(__imp__XamUniSortCmpString, dash::content::XamUniSortCmpString_entry)
REX_HOOK(__imp__XamContentCreateEnumerator, dash::content::XamContentCreateEnumerator_entry)

// Fonctions encore en stub : journalisées (r3 conservé, comme REX_EXPORT_STUB).
#define DASH_LOG_STUB(name)                                                                              \
  REX_HOOK_RAW(__imp__##name) {                                                                          \
    (void)base;                                                                                          \
    REXLOG_INFO("[dash] " #name "(r3={:#x} r4={:#x} r5={:#x} r6={:#x} r7={:#x} r8={:#x}) STUB", ctx.r3.u64, \
                ctx.r4.u64, ctx.r5.u64, ctx.r6.u64, ctx.r7.u64, ctx.r8.u64);                             \
  }
DASH_LOG_STUB(XamContentGetAttributesInternal)
DASH_LOG_STUB(XamContentResolveInternal)
DASH_LOG_STUB(XamContentGetDefaultDevice)
DASH_LOG_STUB(XamContentOpenFileInternal)
