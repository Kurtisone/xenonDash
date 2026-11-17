// xenondash common - stubs noyau dont l'initialisation de XAM a besoin (palier N3+).
//
// XAM recompilé démarre une trentaine de threads qui appellent des fonctions noyau laissées en
// REX_EXPORT_STUB par le SDK. Le stub renvoie r3 inchangé, c'est-à-dire une valeur arbitraire :
// l'appelant prend alors un chemin imprévisible. Ces hooks répondent de façon déterministe, dans
// la ligne du projet (aucune clé de console fabriquée) :
//
//  - HalSendSMCMessage   : message au contrôleur de management (SMC). Rien à piloter sur un PC
//                          émulé : succès, buffer de sortie laissé à zéro.
//  - EtxProducerRegister : télémétrie ETW. Succès : XAM continue, aucun journal n'est produit.
//  - XeKeysGetStatus     : état du magasin de clés. Succès : les clés « locales » du palier N4
//                          (XeKeysObfuscate/UnObfuscate de kernel_obfuscate.cpp) sont prêtes.
//
// Signatures relevées dans le code recompilé (dashboards/2.0.13604.0-xam/generated/xam/) :
//   HalSendSMCMessage(r3 = buffer d'entree, r4 = buffer de sortie) ; retour teste `< 0`, donc
//   0 = succes. EtxProducerRegister et XeKeysGetStatus ne sont pas inspectés au-delà de r3.
#include <atomic>
#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

namespace {

std::atomic<long> g_smc_calls{0};
std::atomic<long> g_etx_calls{0};
std::atomic<long> g_keystatus_calls{0};
std::atomic<long> g_nic_calls{0};
std::atomic<long> g_mtpd_calls{0};

}  // namespace

// HalSendSMCMessage(buffer_entree, buffer_sortie) : le SMC n'existe pas sur PC. On renvoie le
// succes et on laisse le buffer de sortie tel quel (XAM l'a zero-rempli avant l'appel).
REX_HOOK_RAW(__imp__HalSendSMCMessage) {
  (void)base;
  if (++g_smc_calls == 1)
    REXLOG_INFO("[smc] HalSendSMCMessage : succes (aucun SMC sur PC)");
  ctx.r3.u64 = 0;
}

// EtxProducerRegister(...) : enregistrement d'un producteur de télémétrie. Rien n'est collecté,
// mais l'échec ferait prendre à XAM un chemin d'erreur inutile.
REX_HOOK_RAW(__imp__EtxProducerRegister) {
  (void)base;
  if (++g_etx_calls == 1)
    REXLOG_INFO("[etx] EtxProducerRegister : succes (télémétrie ignorée)");
  ctx.r3.u64 = 0;
}

// XeKeysGetStatus(...) : etat du magasin de cles. On annonce « pret » sans fabriquer de cle de
// console : ce sont les cles locales du palier N4 (aleatoire, jamais celles de la console) qui
// servent, via XeKeysObfuscate/UnObfuscate (kernel_obfuscate.cpp).
REX_HOOK_RAW(__imp__XeKeysGetStatus) {
  (void)base;
  if (++g_keystatus_calls == 1)
    REXLOG_INFO("[xekeys] XeKeysGetStatus : cles locales pretes (aucune cle de console)");
  ctx.r3.u64 = 0;
}

// KeSetPriorityClassThread(thread, classe) : priorite d'un thread. Sans objet ici (l'ordonnanceur
// est celui de l'hote), mais l'echec ferait echouer la creation de threads de XAM : 14 appels au
// demarrage, le stub le plus sollicite.
REX_HOOK_RAW(__imp__KeSetPriorityClassThread) {
  (void)base;
  ctx.r3.u64 = 1;  // TRUE : la priorite est « acceptee »
}

// EtxConsumerRegister(...) : enregistrement d'un consommateur de telemetrie. Succes, rien n'est lu.
REX_HOOK_RAW(__imp__EtxConsumerRegister) {
  (void)base;
  ctx.r3.u64 = 0;
}

// NicGetLinkState(interface, *lie) : etat du lien reseau. Une console hors ligne a un lien DOWN.
// Le HUD de XAM interroge cette fonction en boucle pendant son initialisation : le stub renvoyait
// r3 inchange (valeur arbitraire), donc un etat aleatoire. On repond explicitement « pas de lien »,
// ce qui est la verite sur une machine sans Xbox LIVE et evite toute attente.
REX_HOOK_RAW(__imp__NicGetLinkState) {
  (void)base;
  if (ctx.r4.u32 && ctx.r4.u32 < 0xF0000000u)
    *reinterpret_cast<uint32_t*>(base + ctx.r4.u32) = 0;  // aucun lien
  if (++g_nic_calls == 1)
    REXLOG_INFO("[net] NicGetLinkState : pas de lien (hors ligne)");
  ctx.r3.u64 = 0;
}

// MtpdGetCurrentDevices(*liste, ...) : peripheriques de stockage portables (lecteurs MTP). Aucun
// sur un PC sans appareil branche : on rend la liste vide avec succes, plutot qu'un r3 arbitraire.
REX_HOOK_RAW(__imp__MtpdGetCurrentDevices) {
  (void)base;
  if (++g_mtpd_calls == 1)
    REXLOG_INFO("[net] MtpdGetCurrentDevices : aucun peripherique (liste vide)");
  ctx.r3.u64 = 0;
}
