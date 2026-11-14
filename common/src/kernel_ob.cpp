// xenondash common - objets du noyau définis par XAM (ObCreateObject, ObInsertObject) pour XAM recompilé.
//
// XAM déclare ses propres types d'objets (descripteur OBJECT_TYPE dans ses données, étiquette 'Nfto') et crée ses
// ordonnanceurs de tâches avec ObCreateObject / ObInsertObject (sub_8187D9A8, sub_818C3CD0, sub_818924D8, sub_81A56A38).
// Le SDK les laisse en stubs (r3 inchangé) : *objet n'est jamais écrit, la fonction de XAM renvoie un objet nul, et
// l'initialisation du sous-système d'audio de XAM échoue (E_FAIL de l'application 0xFD).
// Ici : ObCreateObject alloue le corps de l'objet (mémoire invitée du tas système, remise à zéro, précédée d'un
// en-tête de 16 octets : compte de pointeurs, compte de handles, type) ; ObInsertObject lui attribue un handle propre
// à ce fichier ; ObReferenceObjectByHandle et NtClose reconnaissent ces handles et passent les autres au SDK.
// Limites : le corps n'est jamais libéré (ObDereferenceObject du SDK ignore ces objets) et les procédures du type
// (destructeur, fermeture) ne sont pas appelées.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/string/utf8.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/string_utils.h>
#include <rex/system/xobject.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>
#include <rex/types.h>

#include "kernel_sdk_fn.h"

namespace {

constexpr uint32_t kHandleBase = 0xF9000000u;  // le SDK utilise 0xF8000000 + n
constexpr uint32_t kHeader = 16;

std::mutex g_mutex;
std::unordered_map<uint32_t, uint32_t> g_handle_to_body;  // handle -> adresse invitée du corps
std::atomic<uint32_t> g_next_handle{kHandleBase + 4};

uint32_t* Word(uint8_t* base, uint32_t guest) { return reinterpret_cast<uint32_t*>(base + guest); }

}  // namespace

// ObCreateObject(type, attributs, tailleCorps, &objet)
REX_HOOK_RAW(__imp__ObCreateObject) {
  const uint32_t type = ctx.r3.u32, size = ctx.r5.u32, out = ctx.r6.u32;
  const uint32_t raw = REX_KERNEL_MEMORY()->SystemHeapAlloc(size + kHeader);
  if (!raw) {
    if (out) *Word(base, out) = 0;
    ctx.r3.u64 = 0xC0000017u;  // STATUS_NO_MEMORY
    return;
  }
  uint8_t* p = base + raw;
  for (uint32_t i = 0; i < size + kHeader; ++i) p[i] = 0;
  *Word(base, raw) = __builtin_bswap32(1);      // compte de pointeurs
  *Word(base, raw + 8) = __builtin_bswap32(type);
  const uint32_t body = raw + kHeader;
  if (out) *Word(base, out) = __builtin_bswap32(body);
  ctx.r3.u64 = 0;
}

// ObInsertObject(objet, attributs, biaisPointeurs, &handle)
REX_HOOK_RAW(__imp__ObInsertObject) {
  const uint32_t body = ctx.r3.u32, out = ctx.r6.u32;
  const uint32_t handle = g_next_handle.fetch_add(4);
  {
    std::lock_guard lock(g_mutex);
    g_handle_to_body[handle] = body;
  }
  if (out) *Word(base, out) = __builtin_bswap32(handle);
  ctx.r3.u64 = 0;
}

// ObReferenceObjectByHandle(handle, type, &objet)
REX_HOOK_RAW(__imp__ObReferenceObjectByHandle) {
  static PPCFunc* sdk = dash::SdkFunction("__imp__ObReferenceObjectByHandle");
  const uint32_t handle = ctx.r3.u32, out = ctx.r5.u32;
  if (handle >= kHandleBase + 4 && handle < kHandleBase + 0x01000000u) {
    uint32_t body = 0;
    {
      std::lock_guard lock(g_mutex);
      auto it = g_handle_to_body.find(handle);
      if (it != g_handle_to_body.end()) body = it->second;
    }
    if (body) {
      if (out) *Word(base, out) = __builtin_bswap32(body);
      ctx.r3.u64 = 0;
      return;
    }
  }
  sdk(ctx, base);
}

// NtClose(handle)
REX_HOOK_RAW(__imp__NtClose) {
  static PPCFunc* sdk = dash::SdkFunction("__imp__NtClose");
  const uint32_t handle = ctx.r3.u32;
  if (handle >= kHandleBase + 4 && handle < kHandleBase + 0x01000000u) {
    std::lock_guard lock(g_mutex);
    if (g_handle_to_body.erase(handle)) {
      ctx.r3.u64 = 0;
      return;
    }
  }
  sdk(ctx, base);
}

// ObReferenceObject(objet) : le SDK en fait un stub, alors que ObDereferenceObject libère bien un handle (ReleaseHandle).
// XAM recompilé appelle ObReferenceObject sur des objets qu'il garde (fils, événements) puis ObDereferenceObject à la fin :
// sans le +1, le compte de handles tombe à zéro trop tôt, l'objet natif est détruit et un fil encore en cours de démarrage
// lit son XThread libéré (SIGSEGV dans XThread::Create()::$_0 sur `thread_->set_name`, 1 démarrage sur 6, gdb).
// Ici : un objet déjà associé à un objet natif (en-tête signé par le SDK) reçoit RetainHandle ; les corps de ce fichier
// et les mémoires sans objet natif sont laissés tels quels.
REX_HOOK_RAW(__imp__ObReferenceObject) {
  const uint32_t ea = ctx.r3.u32;
  if (!ea) return;
  {
    std::lock_guard lock(g_mutex);
    for (const auto& [h, body] : g_handle_to_body) {
      if (body == ea) return;
    }
  }
  auto* header = reinterpret_cast<rex::system::X_DISPATCH_HEADER*>(base + ea);
  auto lock = rex::thread::global_critical_region::AcquireDirect();
  if (static_cast<uint32_t>(header->wait_list_flink) != rex::system::kXObjSignature) return;
  auto object = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XObject>(header->wait_list_blink);
  if (object) object->RetainHandle();
}

// ExCreateThread avec le drapeau 0x80 : la console rend le POINTEUR de l'objet fil (et non un handle) avec une référence
// que l'appelant libère par ObDereferenceObject. Le SDK ne prend pas cette référence : l'ObDereferenceObject de XAM
// libère alors l'auto-référence du fil (RetainHandle dans XThread::Create), l'XThread est détruit pendant que son fil hôte
// démarre ou se termine (SIGSEGV dans XThread::Create()::$_0 sur `thread_` nul, ou dans Thread::Exit ; 1 démarrage sur 6).
// Ici : après l'appel du SDK, une référence de handle supplémentaire est prise pour ce cas.
REX_HOOK_RAW(__imp__ExCreateThread) {
  static PPCFunc* sdk = dash::SdkFunction("__imp__ExCreateThread");
  const uint32_t handle_ptr = ctx.r3.u32, flags = ctx.r9.u32;  // 7e argument : r9
  sdk(ctx, base);
  if (ctx.r3.u32 != 0 || !handle_ptr) return;
  const uint32_t out = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + handle_ptr));
  rex::system::object_ref<rex::system::XObject> object;
  if (flags & 0x80) {
    object = rex::system::XObject::GetNativeObject<rex::system::XObject>(REX_KERNEL_STATE(), base + out);
  } else {
    object = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XObject>(out);
  }
  if (!object) return;
  if (flags & 0x80) object->RetainHandle();
  // Le fil n'est jamais détruit : XThread::Exit du SDK libère le dernier handle puis appelle Thread::Exit sur le
  // PosixThread que ~XThread vient de supprimer (use-after-free constaté par gdb : ~XThread dans XThread::Exit), et un
  // NtClose du créateur avant le démarrage du fil hôte le prive de son XThread. Un compte de pointeurs en plus (jamais
  // rendu) évite les deux ; fuite : quelques centaines d'octets par fil.
  object->Retain();
}

// ObDereferenceObject(objet) : un fil qui se libère lui-même (code de démarrage/sortie de fil du Dashboard et de XAM, appelé
// juste avant ExTerminateThread) libérerait une seconde fois l'auto-référence que XThread::Exit du SDK libère déjà :
// l'XThread est détruit pendant que son fil hôte démarre ou sort (SIGSEGV sporadique, 1 démarrage sur 6, constaté par gdb :
// `thread_` nul dans XThread::Create()::$_0, ou Thread::Exit). Un fil qui déréférence son propre objet est ignoré.
REX_HOOK_RAW(__imp__ObDereferenceObject) {
  static PPCFunc* sdk = dash::SdkFunction("__imp__ObDereferenceObject");
  const uint32_t ea = ctx.r3.u32;
  if (ea && ea != 0xDEADF00D) {
    if (auto* self = rex::system::XThread::GetCurrentThread(); self && self->guest_object() == ea) {
      static std::atomic<int> n{0};
      if (n.fetch_add(1) < 3)
        REXLOG_INFO("[kernel] ObDereferenceObject(0x{:08X}) par le fil lui-même (lr=0x{:08X}) : ignoré", ea,
                    static_cast<uint32_t>(ctx.lr));
      ctx.r3.u64 = 0;
      return;
    }
  }
  sdk(ctx, base);
}

// --- Liens symboliques de XAM (\System??\__tmp0000000B: -> \Device\Harddisk0\Partition1\) ------------------------------------
//
// XAM monte ses périphériques de contenu sous un nom temporaire `\System??\__tmpXXXXXXXX:` (espace de noms DOS du processus
// système) puis ouvre `__tmpXXXXXXXX:\Content\...` sans préfixe. Le SDK retire seulement `\??\` : le lien était enregistré
// sous `\System??\__tmp...:`, jamais retrouvé (« VFS: '__tmp0000000B:\' -> [no device] »), et la création d'un profil
// échouait en ERROR_FILE_NOT_FOUND (palier N4). Ici : `\System??\` et `\??\` sont retirés à la création et à la suppression.
namespace {

std::string SymlinkName(uint8_t* base, uint32_t ansi_ptr, bool strip_prefix) {
  std::string s(rex::system::util::TranslateAnsiStringAddress(REX_KERNEL_MEMORY(), ansi_ptr));
  s = rex::string::trim_string(s);
  if (strip_prefix) {
    for (const char* prefix : {"\\System??\\", "\\??\\"}) {
      const size_t n = std::strlen(prefix);
      if (s.size() >= n && rex::string::utf8_starts_with_case(s, prefix)) {
        s.erase(0, n);
        break;
      }
    }
  }
  (void)base;
  return rex::string::utf8_canonicalize_guest_path(s);
}

}  // namespace

REX_HOOK_RAW(__imp__ObCreateSymbolicLink) {
  const std::string path = SymlinkName(base, ctx.r3.u32, true), target = SymlinkName(base, ctx.r4.u32, false);
  ctx.r3.u64 = REX_KERNEL_FS()->RegisterSymbolicLink(path, target) ? 0u : 0xC0000001u;  // STATUS_UNSUCCESSFUL
}

REX_HOOK_RAW(__imp__ObDeleteSymbolicLink) {
  const std::string path = SymlinkName(base, ctx.r3.u32, true);
  ctx.r3.u64 = REX_KERNEL_FS()->UnregisterSymbolicLink(path) ? 0u : 0xC0000001u;
}

// ObTranslateSymbolicLink(OBJECT_ATTRIBUTES* nom, ANSI_STRING* résultat) : nom d'un lien (ou d'un chemin qui commence par un lien)
// -> chemin réel. Le SDK en fait un stub : XAM (sub_8185DD80) obtenait un chemin vide et ouvrait '' pour créer son paquet.
REX_HOOK_RAW(__imp__ObTranslateSymbolicLink) {
  constexpr uint32_t kStatusNameNotFound = 0xC0000034u, kStatusBufferTooSmall = 0xC0000023u;
  const uint32_t attrs = ctx.r3.u32, out = ctx.r4.u32;
  if (!attrs || !out) {
    ctx.r3.u64 = 0xC000000Du;  // STATUS_INVALID_PARAMETER
    return;
  }
  const std::string path = SymlinkName(base, __builtin_bswap32(*Word(base, attrs + 4)), true);
  // Le lien est le préfixe « <nom>: » (les noms de montage de XAM : __tmp0000000B:, _rand...:, ...) ; le reste est conservé.
  // Un chemin absolu (« \Device\... ») sans lien est déjà réel : renvoyé tel quel.
  std::string resolved, target;
  const size_t colon = path.find(':');
  if (colon != std::string::npos && path.find('\\') > colon) {
    if (!REX_KERNEL_FS()->FindSymbolicLink(path.substr(0, colon + 1), target)) {
      ctx.r3.u64 = kStatusNameNotFound;
      return;
    }
    resolved = target + path.substr(colon + 1);
  } else if (!path.empty() && path[0] == '\\') {
    resolved = path;
  } else {
    ctx.r3.u64 = kStatusNameNotFound;
    return;
  }
  const uint16_t max = __builtin_bswap16(*reinterpret_cast<uint16_t*>(base + out + 2));
  const uint32_t buf = __builtin_bswap32(*Word(base, out + 4));
  if (resolved.size() > max) {
    ctx.r3.u64 = kStatusBufferTooSmall;
    return;
  }
  std::memcpy(base + buf, resolved.data(), resolved.size());
  *reinterpret_cast<uint16_t*>(base + out) = __builtin_bswap16(static_cast<uint16_t>(resolved.size()));
  ctx.r3.u64 = 0;
}

// NtOpenSymbolicLinkObject(&handle, OBJECT_ATTRIBUTES*) : XAM interroge ses liens sous la forme `\System??\<nom>:` ; le SDK ne
// retire que `\??\` après avoir refusé tout chemin qui contient un `?` (« objet invalide », 0xC0000033 : XAM ne retrouvait pas le
// périphérique de son conteneur et ne le fermait jamais : l'en-tête du paquet n'était ni écrit ni signé). Le nom est réécrit sans
// préfixe (`<nom>:`, comme l'enregistre ObCreateSymbolicLink ci-dessus ; copie sous le pointeur de pile de l'appelant) avant le
// traitement du SDK.
REX_HOOK_RAW(__imp__NtOpenSymbolicLinkObject) {
  static PPCFunc* sdk = dash::SdkFunction("__imp__NtOpenSymbolicLinkObject");
  const uint32_t attrs = ctx.r4.u32;
  if (attrs) {
    const uint32_t name_ptr = __builtin_bswap32(*Word(base, attrs + 4));
    const std::string name(rex::system::util::TranslateAnsiStringAddress(REX_KERNEL_MEMORY(), name_ptr));
    constexpr char kPrefix[] = "\\System??\\";
    if (name.size() > sizeof(kPrefix) - 1 && rex::string::utf8_starts_with_case(name, kPrefix)) {
      const std::string fixed = name.substr(sizeof(kPrefix) - 1);
      const uint32_t tmp = (ctx.r1.u32 - 0x400u) & ~0xFu;
      uint8_t* t = base + tmp;
      *reinterpret_cast<uint16_t*>(t) = __builtin_bswap16(static_cast<uint16_t>(fixed.size()));
      *reinterpret_cast<uint16_t*>(t + 2) = __builtin_bswap16(static_cast<uint16_t>(fixed.size() + 1));
      *Word(base, tmp + 4) = __builtin_bswap32(tmp + 16);
      std::memcpy(t + 16, fixed.c_str(), fixed.size() + 1);
      *Word(base, attrs + 4) = __builtin_bswap32(tmp);
    }
  }
  sdk(ctx, base);
}
