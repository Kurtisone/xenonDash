// Diagnostic « Play Game » : journal à l'entrée de sub_928460F8 (Game Library), sub_92238C18 (r3 = contenu, +0x140 = TitleID)
// et sub_92237F30 (lance XamContentLaunchImageInternalEx). Aucun changement de comportement.

#include <cstdint>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

extern "C" void __imp__sub_928460F8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_92238C18(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_92237F30(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_92236600(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_921AF178(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_92235F40(PPCContext& __restrict, uint8_t*);

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
  const uint32_t r5 = ctx.r5.u32, r6 = ctx.r6.u32;
  const uint32_t w506 = r3 ? __builtin_bswap16(*reinterpret_cast<uint16_t*>(base + r3 + 506)) : 0;
  REXLOG_INFO("[play-trace] sub_92237F30 entree +506={:04X} r5={:08X} r6={:08X}", w506, r5, r6);
  __imp__sub_92237F30(ctx, base);
  REXLOG_INFO("[play-trace] sub_92237F30 sortie r3={:08X}", ctx.r3.u32);
}

extern "C" void sub_92236600(PPCContext& __restrict ctx, uint8_t* base) {
  REXLOG_INFO("[play-trace] entree sub_92236600");
  __imp__sub_92236600(ctx, base);
  REXLOG_INFO("[play-trace] sortie sub_92236600 r3={:08X}", ctx.r3.u32);
}

extern "C" void sub_921AF178(PPCContext& __restrict ctx, uint8_t* base) {
  REXLOG_INFO("[play-trace] entree sub_921AF178");
  __imp__sub_921AF178(ctx, base);
  REXLOG_INFO("[play-trace] sortie sub_921AF178 r3={:08X}", ctx.r3.u32);
}

extern "C" void sub_92235F40(PPCContext& __restrict ctx, uint8_t* base) {
  REXLOG_INFO("[play-trace] entree sub_92235F40");
  __imp__sub_92235F40(ctx, base);
  REXLOG_INFO("[play-trace] sortie sub_92235F40 r3={:08X}", ctx.r3.u32);
}
