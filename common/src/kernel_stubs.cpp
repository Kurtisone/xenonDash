// xenondash common - imports noyau/XAM manquants dans le runtime ReXGlue.
//
// - XUsbcam* : xboxkrnl_usbcam.cpp est exclu de la compilation du SDK
//   (src/kernel/CMakeLists.txt). On reprend son comportement (d'après Xenia) :
//   Create réussit, GetState indique "pas de caméra", le reste échoue proprement.
// (sub_92739D9C, spécifique à 6770, est dans dashboards/2.0.6770.0/src/version_stubs.cpp.)

#include <chrono>

#include <rex/hook.h>
#include <rex/types.h>

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

}  // namespace dash::stubs

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
