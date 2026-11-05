// dash - hooks de diagnostic insérés dans le code recompilé (voir [[midasm_hook]] dans dash_overrides.toml).

#include <string>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

namespace {

// Lit une chaîne UTF-16 big-endian dans la mémoire invitée (ASCII seulement, pour le log).
std::string ReadGuestWString(uint32_t guest_address, size_t max_chars = 128) {
  std::string out;
  if (!guest_address) return "<null>";
  auto* p = rex::system::kernel_memory()->TranslateVirtual<const uint8_t*>(guest_address);
  for (size_t i = 0; i < max_chars; i++) {
    uint16_t c = (uint16_t(p[i * 2]) << 8) | p[i * 2 + 1];
    if (!c) break;
    out += (c < 0x80) ? char(c) : '?';
  }
  return out;
}

}  // namespace

void DiagXuiClassNotFound(PPCRegister& r24) {
  REXKRNL_ERROR("[diag] XUI : classe introuvable '{}' (0x{:08X})", ReadGuestWString(r24.u32), r24.u32);
}

// Hooks génériques : journalisent un ou deux registres à une adresse donnée.
#define DIAG_REG1(name, label, ra)                                                       \
  void name(PPCRegister& ra) { REXKRNL_INFO("[diag] {} " #ra "=0x{:08X}", label, ra.u32); }
#define DIAG_REG2(name, label, ra, rb)                                                   \
  void name(PPCRegister& ra, PPCRegister& rb) {                                           \
    REXKRNL_INFO("[diag] {} " #ra "=0x{:08X} " #rb "=0x{:08X}", label, ra.u32, rb.u32);   \
  }

DIAG_REG1(DiagAudioAlloc, "audio factory 9235FBA0: alloc 9235F970 ->", r3)
DIAG_REG2(DiagAudioInnerCall, "audio factory 9235FBA0: appel indirect", r11, r4)
DIAG_REG1(DiagAudioInnerRet, "audio factory 9235FBA0: retour indirect ->", r3)
DIAG_REG1(DiagAudioOuterRet, "audio init 925A86F8: fabrique ->", r3)
