// xenondash common - imports noyau/XAM manquants dans le runtime ReXGlue.
//
// - XUsbcam* : xboxkrnl_usbcam.cpp est exclu de la compilation du SDK
//   (src/kernel/CMakeLists.txt). On reprend son comportement (d'après Xenia) :
//   Create réussit, GetState indique "pas de caméra", le reste échoue proprement.
// (sub_92739D9C, spécifique à 6770, est dans dashboards/2.0.6770.0/src/version_stubs.cpp.)

#include <atomic>
#include <chrono>

#include <rex/hook.h>
#include <rex/types.h>

#include "kernel_sdk_fn.h"
#include <rex/system/kernel_state.h>
#include <rex/system/util/string_utils.h>
#include <rex/system/xobject.h>

namespace dash::stubs {

constexpr u32 kErrorDeviceNotConnected = 0x48F;  // ERROR_DEVICE_NOT_CONNECTED

u32 XUsbcamCreate_entry(u32 buffer, u32 buffer_size, u32 unk) {
  (void)buffer; (void)buffer_size; (void)unk;
  return 0;  // X_STATUS_SUCCESS
}

u32 XUsbcamGetState_entry() {
  return 0;  // 0 = non connectée
}

// KeGetImagePageTableEntry : le Dashboard NXE (17559) valide ses pointeurs de code avec le bit 0 (« page valide »)
// de l'entrée de table de pages ; l'implémentation du SDK y met l'indice de page (bit 0 aléatoire) -> KeBugCheck(0xF7).
// On renvoie une entrée valide pour toute adresse d'image XEX (0x80000000-0x9FFFFFFF).
u32 KeGetImagePageTableEntry_entry(u32 address) {
  if (address < 0x80000000u || address > 0x9FFFFFFFu) return 0;
  return (0x40000000u | ((address >> 12) << 1) | 1u) & 0x400FFFFFu;
}

// KeQueryInterruptTime : temps depuis le démarrage en unités de 100 ns. Le SDK en fait un stub (0) : le Dashboard NXE
// 17559 boucle alors indéfiniment sur son horloge (sub_921AFF68).
u64 KeQueryInterruptTime_entry() {
  using namespace std::chrono;
  return static_cast<u64>(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count() / 100);
}

// Fonctions XAM que le SDK laisse en « STUB » (r3 inchangé = valeur de retour aléatoire). Le Dashboard NXE/Metro (17559)
// les interroge en boucle (état Kinect, contexte du Dashboard, langue) : on renvoie des valeurs saines.
std::atomic<u32> g_dash_context{0};

}  // namespace dash::stubs

// XamGetLanguage() : langue du système (XC_LANGUAGE_ENGLISH = 1).
REX_HOOK_RAW(__imp__XamGetLanguage) { (void)base; ctx.r3.u64 = 1; }
// XamSetDashContext(ctx) / XamGetDashContext() : contexte courant du Dashboard (conservé tel quel).
REX_HOOK_RAW(__imp__XamSetDashContext) { (void)base; dash::stubs::g_dash_context = ctx.r3.u32; ctx.r3.u64 = 0; }
REX_HOOK_RAW(__imp__XamGetDashContext) { (void)base; ctx.r3.u64 = dash::stubs::g_dash_context.load(); }
// XamGetLanguageLocaleFallbackString(langue) : pointeur vers une chaîne large (UTF-16 big-endian) « en-US ».
REX_HOOK_RAW(__imp__XamGetLanguageLocaleFallbackString) {
  static std::atomic<u32> addr{0};
  u32 a = addr.load();
  if (!a) {
    static const char16_t kStr[] = u"en-US";
    a = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(kStr));
    auto* dst = reinterpret_cast<uint16_t*>(base + a);
    for (size_t i = 0; i < sizeof(kStr) / 2; ++i) dst[i] = __builtin_bswap16(static_cast<uint16_t>(kStr[i]));
    addr = a;
  }
  ctx.r3.u64 = a;
}
// Polices du Dashboard NXE/Metro : XamGetLanguageTypeface(langue, taille, tampon) écrit le localisateur de la police de
// la langue ; XamGetLanguageTypefacePatch(langue) renvoie un pointeur vers le localisateur du correctif. La seule police
// de la mise à jour 17559 présente dans la flash est SegoeXbox-Light.xtt (media:\) : on l'utilise pour toutes les langues.
namespace dash::stubs {
constexpr char16_t kTypefaceLocator[] = u"file://media:/SegoeXbox-Light.xtt";
inline void PutWide(uint8_t* base, u32 addr, const char16_t* s, size_t n) {
  auto* dst = reinterpret_cast<uint16_t*>(base + addr);
  for (size_t i = 0; i < n; ++i) dst[i] = __builtin_bswap16(static_cast<uint16_t>(s[i]));
}
}  // namespace dash::stubs
REX_HOOK_RAW(__imp__XamGetLanguageTypeface) {
  const u32 size = ctx.r4.u32, out = ctx.r5.u32;
  constexpr size_t n = sizeof(dash::stubs::kTypefaceLocator) / 2;  // avec le zéro final
  if (out && size >= n) dash::stubs::PutWide(base, out, dash::stubs::kTypefaceLocator, n);
  ctx.r3.u64 = 0;
}
REX_HOOK_RAW(__imp__XamGetLanguageTypefacePatch) {
  static std::atomic<u32> addr{0};
  u32 a = addr.load();
  if (!a) {
    constexpr size_t n = sizeof(dash::stubs::kTypefaceLocator) / 2;
    a = REX_KERNEL_MEMORY()->SystemHeapAlloc(n * 2);
    dash::stubs::PutWide(base, a, dash::stubs::kTypefaceLocator, n);
    addr = a;
  }
  ctx.r3.u64 = a;
}
// Pas de Kinect, pas de mode d'automatisation, pas de mode de préproduction.
REX_HOOK_RAW(__imp__XamIsNuiAutomationEnabled) { (void)base; ctx.r3.u64 = 0; }
REX_HOOK_RAW(__imp__XamNuiIsDeviceReady) { (void)base; ctx.r3.u64 = 0; }
REX_HOOK_RAW(__imp__XamGetStagingMode) { (void)base; ctx.r3.u64 = 0; }
// IoDismountVolumeByName(nom) : démontage d'un volume. Le stub du SDK renvoie une valeur non nulle ; la machine d'état
// du stockage de XAM (thread système) le rappelle alors sans fin. Il n'y a pas de volume à démonter : STATUS_SUCCESS.
REX_HOOK_RAW(__imp__IoDismountVolumeByName) { (void)base; ctx.r3.u64 = 0; }

// DrvGetContentStorageNotification(tampon) : le SDK en fait un stub qui laisse r3 inchangé. Le thread de stockage de XAM
// (sub_8185C5E0) l'appelle en boucle tant que r3 >= 0 : il tournait à 100 % d'un cœur sans jamais signaler la fin de
// l'énumération initiale, et le fil d'initialisation de XAM (sub_81859780, attente de l'événement 0x81BE0DF8) restait bloqué.
// Aucun périphérique de stockage USB : échec (STATUS_UNSUCCESSFUL), la boucle se termine et signale l'événement.
REX_HOOK_RAW(__imp__DrvGetContentStorageNotification) { (void)base; ctx.r3.u64 = 0xC0000001u; }

// ObReferenceObjectByName(PANSI_STRING nom, attributs, type, contexte, *objet) : le SDK prend `nom` pour une chaîne C
// (l'adresse de l'ANSI_STRING est lue comme du texte : nom vide, recherche toujours en échec). XAM (threads système)
// appelle ensuite ObDereferenceObject(*objet) sans contrôler le statut : *objet n'est pas écrit et garde une valeur
// ancienne (0xBEBEBEBE, pointeurs d'objets précédents). Ce déréférencement décrémente le compte de handles d'un objet
// quelconque (dont le thread en cours de création du Dashboard, qui est alors détruit). Ici : nom lu comme ANSI_STRING,
// et *objet = 0 en cas d'échec.
REX_HOOK_RAW(__imp__ObReferenceObjectByName) {
  const u32 name_addr = ctx.r3.u32, out = ctx.r7.u32;
  const std::string_view name = rex::system::util::TranslateAnsiStringAddress(REX_KERNEL_MEMORY(), name_addr);
  constexpr uint32_t kStatusNameNotFound = 0xC0000034u;  // X_STATUS_OBJECT_NAME_NOT_FOUND
  uint32_t handle = 0xFFFFFFFFu;                         // X_INVALID_HANDLE_VALUE
  if (REX_KERNEL_OBJECTS()->GetObjectByName(name, &handle) == 0) {  // X_STATUS_SUCCESS
    auto object = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XObject>(handle);
    if (object) {
      object->RetainHandle();  // l'appelant prend la référence (libérée par ObDereferenceObject)
      if (out) *reinterpret_cast<uint32_t*>(base + out) = __builtin_bswap32(object->guest_object());
      ctx.r3.u64 = 0;
      return;
    }
  }
  if (out) *reinterpret_cast<uint32_t*>(base + out) = 0;
  ctx.r3.u64 = kStatusNameNotFound;
}

// NetDll_WSAStartupEx(appelant, version, &WSADATA, drapeaux) : le SDK n'implémente que NetDll_WSAStartup (succès, WSADATA rempli) et
// laisse la variante Ex en stub (r3 inchangé = 2, soit un échec). signin.xex l'appelle par XexGetProcedureAddress(xam.xex, 36) :
// il quittait aussitôt avec 80004005 (« This feature is temporarily unavailable », palier N4). Même traitement que WSAStartup :
// démarrage de Winsock sans réseau (les opérations de socket échouent ensuite d'elles-mêmes).
REX_HOOK_RAW(__imp__NetDll_WSAStartupEx) {
  static PPCFunc* fn = dash::SdkFunction("__imp__NetDll_WSAStartup");
  fn(ctx, base);
}

REX_EXPORT(__imp__XUsbcamCreate, dash::stubs::XUsbcamCreate_entry)
REX_EXPORT(__imp__XUsbcamGetState, dash::stubs::XUsbcamGetState_entry)
REX_EXPORT_STUB_RETURN(__imp__XUsbcamDestroy, 0)
REX_EXPORT_STUB_RETURN(__imp__XUsbcamGetConfig, dash::stubs::kErrorDeviceNotConnected)
REX_EXPORT_STUB_RETURN(__imp__XUsbcamSetConfig, dash::stubs::kErrorDeviceNotConnected)
REX_EXPORT_STUB_RETURN(__imp__XUsbcamSetView, dash::stubs::kErrorDeviceNotConnected)
REX_EXPORT_STUB_RETURN(__imp__XUsbcamSetCaptureMode, dash::stubs::kErrorDeviceNotConnected)
REX_EXPORT_STUB_RETURN(__imp__XUsbcamReadFrame, dash::stubs::kErrorDeviceNotConnected)
REX_EXPORT(__imp__KeGetImagePageTableEntry, dash::stubs::KeGetImagePageTableEntry_entry)
REX_EXPORT(__imp__KeQueryInterruptTime, dash::stubs::KeQueryInterruptTime_entry)
