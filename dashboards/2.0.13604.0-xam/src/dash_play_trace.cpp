// Diagnostic « Play Game » : journal à l'entrée de sub_928460F8 (Game Library), sub_92238C18 (r3 = contenu, +0x140 = TitleID)
// et sub_92237F30 (lance XamContentLaunchImageInternalEx). Aucun changement de comportement.

#include <cstdint>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

extern "C" void __imp__sub_928460F8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_92238C18(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_92237F30(PPCContext& __restrict, uint8_t*);

extern "C" void sub_928460F8(PPCContext& __restrict ctx, uint8_t* base) {
  REXLOG_INFO("[play-trace] sub_928460F8 r3={:08X}", ctx.r3.u32);
  __imp__sub_928460F8(ctx, base);
}

extern "C" void sub_92238C18(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t r3 = ctx.r3.u32;
  if (r3)
    REXLOG_INFO("[play-trace] sub_92238C18 r3={:08X} TitleID={:08X}", r3,
                __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + r3 + 0x140)));
  else
    REXLOG_INFO("[play-trace] sub_92238C18 r3 nul");
  __imp__sub_92238C18(ctx, base);
}

extern "C" void sub_92237F30(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t r3 = ctx.r3.u32;
  if (r3)
    REXLOG_INFO("[play-trace] sub_92237F30 r3={:08X} TitleID={:08X}", r3,
                __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + r3 + 0x140)));
  else
    REXLOG_INFO("[play-trace] sub_92237F30 r3 nul");
  __imp__sub_92237F30(ctx, base);
}
