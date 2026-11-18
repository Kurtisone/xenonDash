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
#include "dash_xam_runtime.h"
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

namespace {

std::atomic<long> g_smc_calls{0};
std::atomic<long> g_etx_calls{0};
std::atomic<long> g_keystatus_calls{0};
std::atomic<long> g_nic_calls{0};
std::atomic<long> g_mtpd_calls{0};
std::atomic<long> g_xexstart_calls{0};
std::atomic<long> g_powerup_calls{0};

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

// XexStartExecutable(entree) : XAM charge un executif (XexLoadExecutable) puis appelle ceci, puis
// cree un thread systeme. Sur console le noyau execute la fonction passee en r3. Le stub ne faisait
// rien : l'executif charge n'etait jamais demarre.
//
// On l'execute sur un thread systeme, via l'infrastructure du projet (dash_xam_runtime), qui gere
// deja le contexte de fil invite et le type de processus attendu par XAM.
REX_HOOK_RAW(__imp__XexStartExecutable) {
  const uint32_t entry = ctx.r3.u32;
  if (++g_xexstart_calls <= 3)
    REXLOG_INFO("[xex] XexStartExecutable(0x{:08X}) : demarrage sur un fil systeme", entry);
  if (entry) {
    if (auto* ks = rex::system::kernel_state()) {
      dash::xamrt::RunInSystemProcessAsync(ks, [ks, entry] {
        auto* ts = rex::runtime::ThreadState::Get();
        if (!ts) return 0;
        ks->function_dispatcher()->Execute(ts, entry, nullptr, 0);
        return 0;
      });
    }
  }
  ctx.r3.u64 = 0;
}

// XexLoadExecutable(nom, drapeaux, &section, &base) : charge un executif. Stub du SDK. XAM l'appelle
// juste avant XexStartExecutable, sur le chemin du demarrage d'un executif (voir la chaine du HUD).
//
// On journalise d'abord ce qui est DEMANDE : sans cela on ignore quel fichier XAM veut demarrer,
// et toute implementation serait a l'aveugle. DASH_TRACE_XEXLOAD=1 trace chaque appel.
REX_HOOK_RAW(__imp__XexLoadExecutable) {
  const uint32_t name_ptr = ctx.r3.u32;
  std::string name;
  if (name_ptr && name_ptr < 0xF0000000u) {
    // X_ANSI_STRING : u16 longueur, u16 longueur max, u32 pointeur.
    const uint32_t len = __builtin_bswap16(*reinterpret_cast<uint16_t*>(base + name_ptr));
    const uint32_t str = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + name_ptr + 4));
    if (str && str < 0xF0000000u && len > 0 && len < 512)
      name.assign(reinterpret_cast<const char*>(base + str), len);
  }
  static std::atomic<long> n{0};
  static const bool trace = std::getenv("DASH_TRACE_XEXLOAD") != nullptr;
  if (trace || ++n <= 3)
    REXLOG_INFO("[xex] XexLoadExecutable(\"{}\", drapeaux={:X}) -> non implemente (stub du SDK)", name,
                ctx.r4.u32);
  // Sorties laissees a zero : l'appelant teste le statut de retour et n'utilise pas les pointeurs
  // sur echec.
  if (ctx.r5.u32 && ctx.r5.u32 < 0xF0000000u) *reinterpret_cast<uint32_t*>(base + ctx.r5.u32) = 0;
  if (ctx.r6.u32 && ctx.r6.u32 < 0xF0000000u) *reinterpret_cast<uint32_t*>(base + ctx.r6.u32) = 0;
  ctx.r3.u64 = 0xC0000135u;  // STATUS_DLL_NOT_FOUND : honnete, on n'a rien charge
}

// HalGetPowerUpCause(&etat) : pourquoi la console a demarre (bouton, eject, reveil reseau...).
// Remplit une structure que XAM lit ensuite octet par octet.
//
// Pourquoi c'est important : juste apres l'appel, XAM teste le bit 0 de l'octet a l'offset 4
// (sub_81892A60 : lbz r11,4(r11) puis clrlwi r3,r11,31). Si ce bit est a 0, XAM appelle
// VdDisplayFatalError(0x00012348) — « erreur fatale d'affichage » — mesure dans le journal.
//
// Le stub du SDK ne remplissait rien : la memoire restait a zero, donc le bit valait 0 et XAM
// croyait a une panne d'affichage. On declare ici un demarrage normal (mise sous tension par
// l'utilisateur), ce qui est la verite sur un PC.
REX_HOOK_RAW(__imp__HalGetPowerUpCause) {
  const uint32_t out = ctx.r3.u32;
  if (out && out < 0xF0000000u) {
    // Structure de 8 octets remise a zero, puis :
    //   octet 0    : type de mise sous tension = 1 (bouton / mise sous tension normale)
    //   octet 4    : drapeaux ; le bit 0 doit etre a 1 pour que XAM ne signale pas d'erreur
    //                d'affichage (voir sub_81892A60 dans le code recompilé).
    uint8_t* s = reinterpret_cast<uint8_t*>(base + out);
    for (int i = 0; i < 8; ++i) s[i] = 0;
    // ATTENTION a la polarite, verifiee dans le code recompilé (sub_81872230) :
    //   bl sub_81892A60 ; clrlwi. r11,r3,24 ; beq loc_81872298 ; <errata fatal>
    // c'est-a-dire : si sub_81892A60() == 0, on SAUTE l'erreur. Le bit doit donc rester a 0.
    // Mettre 1 ici PROVOQUE VdDisplayFatalError — erreur que j'ai commise d'abord.
    s[0] = 1;
    s[4] = 0;
  }
  if (++g_powerup_calls <= 3)
    REXLOG_INFO("[hal] HalGetPowerUpCause(0x{:08X}) : mise sous tension normale, octets 0-7 = "
                "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                out, out ? *(base + out + 0) : 0, out ? *(base + out + 1) : 0,
                out ? *(base + out + 2) : 0, out ? *(base + out + 3) : 0,
                out ? *(base + out + 4) : 0, out ? *(base + out + 5) : 0,
                out ? *(base + out + 6) : 0, out ? *(base + out + 7) : 0);
  ctx.r3.u64 = 0;
}

// VdDisplayFatalError(code) : XAM signale une erreur fatale d'affichage. Deux sites l'appellent dans
// xam.xex (0x81872230 : code 0x12348 ; 0x8187A038 : code 0x12237). On ne l'implemente pas — on
// JOURNALISE, pour savoir lequel tire et avec quel code. C'est la mesure qui manquait pour
// distinguer les deux chemins.
REX_HOOK_RAW(__imp__VdDisplayFatalError) {
  (void)base;
  // Journal simple (pas de formatage) : le but est de savoir si ce hook est appele du tout.
  REXLOG_WARN("[vd] VdDisplayFatalError appele (voir code et lr ci-dessous)");
  REXLOG_WARN("[vd]   code={} lr={}", uint32_t(ctx.r3.u32), uint32_t(ctx.lr));
  ctx.r3.u64 = 0;
}
