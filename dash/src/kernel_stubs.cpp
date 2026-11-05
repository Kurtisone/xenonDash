// dash - imports noyau/XAM manquants dans le runtime ReXGlue.
//
// - XUsbcam* : xboxkrnl_usbcam.cpp est exclu de la compilation du SDK
//   (src/kernel/CMakeLists.txt). On reprend son comportement (d'après Xenia) :
//   Create réussit, GetState indique "pas de caméra", le reste échoue proprement.
// - sub_92739D9C : thunk d'import de xam.xex ordinal 2790 (0xAE6), absent des
//   tables d'exports Xenia/ReXGlue. Stub en attendant de l'identifier dans le
//   xam.xex de la NAND.

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

}  // namespace dash::stubs

REX_EXPORT(__imp__XUsbcamCreate, dash::stubs::XUsbcamCreate_entry)
REX_EXPORT(__imp__XUsbcamGetState, dash::stubs::XUsbcamGetState_entry)
REX_EXPORT_STUB_RETURN(__imp__XUsbcamDestroy, 0)
REX_EXPORT_STUB_RETURN(__imp__XUsbcamGetConfig, dash::stubs::kErrorDeviceNotConnected)
REX_EXPORT_STUB_RETURN(__imp__XUsbcamSetConfig, dash::stubs::kErrorDeviceNotConnected)
REX_EXPORT_STUB_RETURN(__imp__XUsbcamSetView, dash::stubs::kErrorDeviceNotConnected)
REX_EXPORT_STUB_RETURN(__imp__XUsbcamSetCaptureMode, dash::stubs::kErrorDeviceNotConnected)
REX_EXPORT_STUB_RETURN(__imp__XUsbcamReadFrame, dash::stubs::kErrorDeviceNotConnected)

// xam.xex ordinal 2790 — inconnu.
REX_STUB_RETURN(sub_92739D9C, 0)
