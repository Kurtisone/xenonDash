// Diagnostic de la boucle d'image du HUD de XAM (Guide invisible, docs/REPRISE.md) : journalise l'entrée de l'initialisation
// et de la boucle du HUD, et le retour du portier sub_818B5028 (ObCreateSymbolicLink). Actif seulement avec
// DASH_HUD_LOOP_TRACE=1 ; ne modifie ni les arguments ni les résultats (chaque fonction appelle son original).
// Même mécanisme que xam_input_patch.cpp : définition forte de sub_<adresse> qui appelle __imp__sub_<adresse>.

#include <cstdint>
#include <cstdlib>

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>

namespace {
bool LoopTraceOn() {
  static const bool on = std::getenv("DASH_HUD_LOOP_TRACE") != nullptr;
  return on;
}

// Champ de garde de la boucle d'image (sub_818A2500 lit 0x81BFCB8C = base 0x81BFCAE8 + 164). Journalise sa valeur.
uint32_t GuardField() {
  auto* p = reinterpret_cast<const uint32_t*>(REX_KERNEL_MEMORY()->virtual_membase() + 0x81BFCB8Cu);
  return __builtin_bswap32(*p);
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
  if (LoopTraceOn()) REXLOG_INFO("[hudloop] retour sub_818A2F90 garde={:08X}", GuardField());
}

extern "C" void __imp__sub_818A2500(PPCContext& __restrict, uint8_t*);
extern "C" void sub_818A2500(PPCContext& __restrict ctx, uint8_t* base) {
  if (LoopTraceOn()) REXLOG_INFO("[hudloop] entree sub_818A2500 lr={:08X} r3={:08X} garde={:08X}", (uint32_t)ctx.lr, ctx.r3.u32, GuardField());
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

// Corps de la boucle d'image du HUD (sub_818A2500) : une entree sans retour dans le journal pointe l'appel qui bloque.
#define HUDLOOP_TRACE_FN(NAME)                                                                             \
  extern "C" void __imp__##NAME(PPCContext& __restrict, uint8_t*);                                         \
  extern "C" void NAME(PPCContext& __restrict ctx, uint8_t* base) {                                        \
    if (LoopTraceOn()) REXLOG_INFO("[hudloop] entree " #NAME " lr={:08X} r3={:08X}", (uint32_t)ctx.lr, ctx.r3.u32); \
    __imp__##NAME(ctx, base);                                                                              \
    if (LoopTraceOn()) REXLOG_INFO("[hudloop] retour " #NAME " r3={:08X}", ctx.r3.u32);                     \
  }
HUDLOOP_TRACE_FN(sub_8186B940)
HUDLOOP_TRACE_FN(sub_8186F2F8)
HUDLOOP_TRACE_FN(sub_81894628)
HUDLOOP_TRACE_FN(sub_8189DAC8)
HUDLOOP_TRACE_FN(sub_81895170)


// Écrivain du champ de garde (+164 := 1) : sub_818A1DF8, sans appelant direct (appel indirect). Journalise ses prérequis.
extern "C" void __imp__sub_818A1DF8(PPCContext& __restrict, uint8_t*);
extern "C" void sub_818A1DF8(PPCContext& __restrict ctx, uint8_t* base) {
  if (LoopTraceOn()) {
    auto rd = [](uint32_t off) {
      return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(REX_KERNEL_MEMORY()->virtual_membase() + 0x81BFCAE8u + off));
    };
    REXLOG_INFO("[hudloop] entree sub_818A1DF8 lr={:08X} +32={:08X} +20={:08X} +68={:08X}", (uint32_t)ctx.lr, rd(32), rd(20), rd(68));
  }
  __imp__sub_818A1DF8(ctx, base);
  if (LoopTraceOn()) REXLOG_INFO("[hudloop] retour sub_818A1DF8 garde={:08X}", GuardField());
}

// Garde de planification du module huduiskin (sub_818A2F90) : si l'un de ces deux appels renvoie un negatif, le fil
// sub_818A2C98 (chargement de huduiskin.xex) n'est jamais planifie. Trace de l'entree et du retour (r3).
HUDLOOP_TRACE_FN(sub_81891CA8)

// Planification du module huduiskin : sub_81891CF0 (planification du fil, cible 0x818A2C98) et le fil lui-meme sub_818A2C98
// (chargement de huduiskin.xex). Si sub_818A2C98 n'entre jamais, la planification ne l'a pas lancee.
HUDLOOP_TRACE_FN(sub_81891CF0)
HUDLOOP_TRACE_FN(sub_818A2C98)

// Garde du chargement de huduiskin (sub_818A2C98) : si sub_8189EBE0 renvoie un negatif, sub_8189CA48 (chargement) est sautee.
HUDLOOP_TRACE_FN(sub_8189EBE0)
HUDLOOP_TRACE_FN(sub_8189CA48)

// Gardes de sub_8189CA48 (chargement huduiskin, graphique) : sub_8194E200 puis sub_81949DE0 ; un negatif fait sortir la fonction.
HUDLOOP_TRACE_FN(sub_8194E200)
HUDLOOP_TRACE_FN(sub_81949DE0)

// Chaine du chargement de huduiskin (sub_818A2C98) : allocation sub_818BB140, puis sub_8186E020, puis sub_8187F8D8 (XexLoadImage suit).
HUDLOOP_TRACE_FN(sub_818BB140)
HUDLOOP_TRACE_FN(sub_8186E020)
HUDLOOP_TRACE_FN(sub_8187F8D8)

// Signal de fin de boot du systeme UI (hypothese) : message 0x80000012/0x80000011 traite par sub_81894D50, pose/retrait du bit
// +176 par sub_8189F050(1/0), ouverture de scene par sub_818A21B0. Journalise masque (+0), +164, +172, +176, +216 a chaque passage.
namespace {
void UiState(const char* tag, uint32_t r3) {
  auto rd = [](uint32_t off) {
    return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(REX_KERNEL_MEMORY()->virtual_membase() + 0x81BFCAE8u + off));
  };
  REXLOG_INFO("[hudloop] {} r3={:08X} masque={:08X} +164={:08X} +172={:08X} +176={:08X} +216={:08X}", tag, r3, rd(0), rd(164), rd(172), rd(176), rd(216));
}
}  // namespace
#define HUDLOOP_STATE_FN(NAME)                                                       \
  extern "C" void __imp__##NAME(PPCContext& __restrict, uint8_t*);                   \
  extern "C" void NAME(PPCContext& __restrict ctx, uint8_t* base) {                  \
    if (LoopTraceOn()) UiState("entree " #NAME, ctx.r3.u32);                         \
    __imp__##NAME(ctx, base);                                                        \
    if (LoopTraceOn()) UiState("retour " #NAME, ctx.r3.u32);                         \
  }
HUDLOOP_STATE_FN(sub_81894D50)
HUDLOOP_STATE_FN(sub_8189F050)
HUDLOOP_STATE_FN(sub_818A21B0)
