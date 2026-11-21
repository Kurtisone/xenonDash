// Diagnostic de la boucle d'image du HUD de XAM (Guide invisible, docs/REPRISE.md) : journalise l'entrée de l'initialisation
// et de la boucle du HUD, et le retour du portier sub_818B5028 (ObCreateSymbolicLink). Actif seulement avec
// DASH_HUD_LOOP_TRACE=1 ; ne modifie ni les arguments ni les résultats (chaque fonction appelle son original).
// Même mécanisme que xam_input_patch.cpp : définition forte de sub_<adresse> qui appelle __imp__sub_<adresse>.

#include <cstdint>
#include <cstdlib>

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

namespace {
bool LoopTraceOn() {
  static const bool on = std::getenv("DASH_HUD_LOOP_TRACE") != nullptr;
  return on;
}
}  // namespace

extern "C" void __imp__sub_81872230(PPCContext& __restrict, uint8_t*);
extern "C" void sub_81872230(PPCContext& __restrict ctx, uint8_t* base) {
  if (LoopTraceOn()) REXLOG_INFO("[hudloop] entree sub_81872230 lr={:08X} r3={:08X}", (uint32_t)ctx.lr, ctx.r3.u32);
  __imp__sub_81872230(ctx, base);
}

extern "C" void __imp__sub_81872460(PPCContext& __restrict, uint8_t*);
extern "C" void sub_81872460(PPCContext& __restrict ctx, uint8_t* base) {
  if (LoopTraceOn()) REXLOG_INFO("[hudloop] entree sub_81872460 lr={:08X} r3={:08X}", (uint32_t)ctx.lr, ctx.r3.u32);
  __imp__sub_81872460(ctx, base);
}

extern "C" void __imp__sub_81891BA8(PPCContext& __restrict, uint8_t*);
extern "C" void sub_81891BA8(PPCContext& __restrict ctx, uint8_t* base) {
  if (LoopTraceOn()) REXLOG_INFO("[hudloop] entree sub_81891BA8 lr={:08X} r3={:08X} r4={:08X}", (uint32_t)ctx.lr, ctx.r3.u32, ctx.r4.u32);
  __imp__sub_81891BA8(ctx, base);
}

extern "C" void __imp__sub_818A2F90(PPCContext& __restrict, uint8_t*);
extern "C" void sub_818A2F90(PPCContext& __restrict ctx, uint8_t* base) {
  if (LoopTraceOn()) REXLOG_INFO("[hudloop] entree sub_818A2F90 lr={:08X} r3={:08X}", (uint32_t)ctx.lr, ctx.r3.u32);
  __imp__sub_818A2F90(ctx, base);
}

extern "C" void __imp__sub_818A2500(PPCContext& __restrict, uint8_t*);
extern "C" void sub_818A2500(PPCContext& __restrict ctx, uint8_t* base) {
  if (LoopTraceOn()) REXLOG_INFO("[hudloop] entree sub_818A2500 lr={:08X} r3={:08X}", (uint32_t)ctx.lr, ctx.r3.u32);
  __imp__sub_818A2500(ctx, base);
}

extern "C" void __imp__sub_818A5A08(PPCContext& __restrict, uint8_t*);
extern "C" void sub_818A5A08(PPCContext& __restrict ctx, uint8_t* base) {
  if (LoopTraceOn()) REXLOG_INFO("[hudloop] entree sub_818A5A08 lr={:08X} r3={:08X}", (uint32_t)ctx.lr, ctx.r3.u32);
  __imp__sub_818A5A08(ctx, base);
}

extern "C" void __imp__sub_818B5028(PPCContext& __restrict, uint8_t*);
extern "C" void sub_818B5028(PPCContext& __restrict ctx, uint8_t* base) {
  if (LoopTraceOn()) REXLOG_INFO("[hudloop] entree sub_818B5028 (portier) lr={:08X} r3={:08X}", (uint32_t)ctx.lr, ctx.r3.u32);
  __imp__sub_818B5028(ctx, base);
  if (LoopTraceOn()) REXLOG_INFO("[hudloop] retour sub_818B5028 (portier) r3={:08X}", ctx.r3.u32);
}

// Boîte de démarrage « Game Error » : XAM envoie au boot le message 0x00020023 à l'application 254 (sub_818C1188 -> sub_8187BA98)
// dès que le bit 0x8 de XeKeysGetStatus est absent. Ce bit commande aussi la boucle d'image du HUD (sub_81872230, r28), qui doit
// donc rester ABSENT. On ne court-circuite que l'envoi du message : sub_818B58E8 (lecture du bit) n'est pas touché.
// MESURE (2026-11-21) : ce wrapper supprime bien le message, mais la boîte reste affichée : elle vient d'un autre appel
// (XamShowMessageBox, ordinal 745). Désactivé par défaut ; DASH_XAM_NO_BOOTBOX=1 l'active pour diagnostic.
extern "C" void __imp__sub_818C1188(PPCContext& __restrict, uint8_t*);
extern "C" void sub_818C1188(PPCContext& __restrict ctx, uint8_t* base) {
  static const bool skip = [] {
    const char* e = std::getenv("DASH_XAM_NO_BOOTBOX");
    return e && e[0] == '1';
  }();
  if (skip) {
    if (LoopTraceOn()) REXLOG_INFO("[hudloop] message 0x00020023 (boîte de démarrage) non envoyé lr={:08X}", (uint32_t)ctx.lr);
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_818C1188(ctx, base);
}
