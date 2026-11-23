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
#include <cstdlib>
#include <cstring>

#include <string>

#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/vfs.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include "dash_xam_runtime.h"
#include "kernel_sdk_fn.h"
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

namespace {

std::atomic<long> g_smc_calls{0};
std::atomic<long> g_etx_calls{0};
std::atomic<long> g_keystatus_calls{0};
std::atomic<long> g_nic_calls{0};
std::atomic<long> g_mtpd_calls{0};
std::atomic<long> g_nicstats_calls{0};
std::atomic<long> g_xexstart_calls{0};
std::atomic<long> g_powerup_calls{0};
std::atomic<long> g_vdswap_calls{0};
std::atomic<long> g_textinput_calls{0};
std::atomic<long> g_natal_calls{0};
std::atomic<long> g_hive_calls{0};
std::atomic<long> g_devstats_calls{0};
std::atomic<long> g_etxlogs_calls{0};

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

// XeKeysGetStatus(uint32_t* status_out) : etat du magasin de cles. On annonce « pret » sans fabriquer de cle de
// console : ce sont les cles locales du palier N4 (aleatoire, jamais celles de la console) qui
// servent, via XeKeysObfuscate/UnObfuscate (kernel_obfuscate.cpp).
// La SORTIE (r3 = pointeur) doit être remplie : laissée à 0, l'absence du bit 3 (0x8) déclenchait l'envoi du
// message 0x00020023 à l'application 254 par XAM au boot (sub_818B8658 -> sub_818C1188) — d'où la boîte modale
// « Game Error » / « Unrecognized Disc » au démarrage (traque du 2026-11-21). Le bit 2 (0x4) est lu par deux
// autres sites. Sur console retail ces contrôles de sécurité sont au vert (status = 0xC), mais la valeur rendue ici est
// 0x4 par défaut (bit 3 absent) : voir ci-dessous, 0xC empêche le HUD de XAM de démarrer.
REX_HOOK_RAW(__imp__XeKeysGetStatus) {
  // Défaut 0x4 (bit 3 absent) : avec 0xC le fil du HUD de XAM (sub_818A2F90 -> sub_818A2500) ne démarre pas et le Guide
  // ne s'affiche jamais ; avec 0x4 il démarre et remplit lui-même l'état UI (mesure du 2026-11-22).
  // DASH_XEKEYS_STATUS=<hex> : valeur de test.
  static const uint32_t status = [] {
    const char* e = std::getenv("DASH_XEKEYS_STATUS");
    return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 16)) : 0x4u;
  }();
  if (ctx.r3.u32) *reinterpret_cast<uint32_t*>(base + ctx.r3.u32) = __builtin_bswap32(status);
  if (++g_keystatus_calls == 1)
    REXLOG_INFO("[xekeys] XeKeysGetStatus : cles locales pretes (aucune cle de console), status=0x{:X}", status);
  ctx.r3.u64 = 0;
}

// XeKeysVerifyRSASignature(r3 = indice de clé publique Microsoft, r4 = condensat SHA-1 de 20 octets, r5 = signature de
// 256 octets) : BOOL. Le stub du SDK renvoyait r3 inchangé (l'indice 0 = FALSE). Appelants observés (mesure du 23/11) :
// la validation de system.manifest par le gestionnaire de paquets de XAM (sub_818BEF98, lr 0x818BF0C0, après sa propre
// comparaison SHA-1 du contenu, 0x8B050015 sinon) et celle des en-têtes des paquets FFFE07DF de la partition système
// étendue (sub_8185A438, lr 0x8185A4C4). Avec FALSE le manifeste est rejeté (0x8B050012) et l'index (0x81C06280 +164)
// reste vide.
// Les clés publiques de Microsoft sont dans l'hyperviseur, absentes ici : la signature n'est pas recalculée, elle est
// acceptée comme le fait déjà le SDK pour ses vérifications sœurs (XeCryptBnDwLePkcs1Verify,
// XeKeysConsoleSignatureVerification). L'intégrité du contenu reste vérifiée par XAM (SHA-1).
REX_HOOK_RAW(__imp__XeKeysVerifyRSASignature) {
  (void)base;
  REXLOG_INFO("[xekeys] XeKeysVerifyRSASignature(cle {}) lr={:08X} : signature acceptee (cle publique absente)",
              ctx.r3.u32, static_cast<uint32_t>(ctx.lr));
  ctx.r3.u64 = 1;
}

// XexLoadImageHeaders(r3 = ANSI_STRING* chemin, r4 = tampon, r5 = taille) : charge l'en-tête XEX2 d'un fichier dans le
// tampon. Le SDK renvoie STATUS_NOT_IMPLEMENTED. Appelant observé : la validation de system.manifest par XAM (sub_81868700,
// pour chaque .xex de \Device\Harddisk0\SystemExtPartition\20352400) ; il retente avec 64 Kio sur STATUS_BUFFER_TOO_SMALL,
// puis compare le SHA-1 des 256 octets de signature (*(tampon+0x10) + 8 : le champ « security_offset » devenu pointeur)
// au condensat du manifeste. Le noyau rend donc l'en-tête avec ce champ relogé ; les autres champs restent des décalages.
// La signature RSA de l'en-tête n'est pas vérifiée (clés publiques absentes, voir XeKeysVerifyRSASignature).
REX_HOOK_RAW(__imp__XexLoadImageHeaders) {
  auto rd32 = [](const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; };
  const uint32_t str = ctx.r3.u32, out = ctx.r4.u32, size = ctx.r5.u32;
  if (!str || !out) {
    ctx.r3.u64 = 0xC000000D;  // STATUS_INVALID_PARAMETER
    return;
  }
  const uint16_t len = uint16_t(base[str] << 8 | base[str + 1]);
  const uint32_t buf = rd32(base + str + 4);
  const std::string path(reinterpret_cast<const char*>(base + buf), len);
  rex::filesystem::Entry* entry = REX_KERNEL_FS()->ResolvePath(path);
  rex::filesystem::File* file = nullptr;
  if (!entry || entry->Open(0x80000000u /*GENERIC_READ*/, &file) != 0 || !file) {
    REXLOG_INFO("[xex] XexLoadImageHeaders({}) : introuvable", path);
    ctx.r3.u64 = 0xC0000034;  // STATUS_OBJECT_NAME_NOT_FOUND
    return;
  }
  uint8_t head[24] = {};
  size_t got = 0;
  uint32_t status = 0;
  if (file->ReadSync({head, sizeof(head)}, 0, &got) != 0 || got != sizeof(head) || std::memcmp(head, "XEX2", 4) != 0) {
    status = 0xC000007B;  // STATUS_INVALID_IMAGE_FORMAT
  } else {
    const uint32_t header_size = rd32(head + 8), security_offset = rd32(head + 16);
    if (header_size > size) {
      status = 0xC0000023;  // STATUS_BUFFER_TOO_SMALL
    } else if (uint64_t(security_offset) + 8 + 256 > header_size) {
      // Signature (security_offset + 8, 256 octets) hors de l'en-tête : calcul en 64 bits (pas de repli de
      // l'addition sur 32 bits pour un security_offset aberrant), refus sans rien lire.
      REXLOG_WARN("[xex] XexLoadImageHeaders({}) : security_offset 0x{:X} + 264 hors de l'en-tête (0x{:X} octets)", path,
                  security_offset, header_size);
      status = 0xC000007B;  // STATUS_INVALID_IMAGE_FORMAT
    } else if (file->ReadSync({base + out, header_size}, 0, &got) != 0 || got != header_size) {
      status = 0xC000007B;
    } else {
      // Seul le champ +16 (security_offset) est relogé en pointeur ; les autres décalages restent tels quels.
      const uint32_t ptr = out + security_offset;
      uint8_t* f = base + out + 16;
      f[0] = ptr >> 24; f[1] = ptr >> 16; f[2] = ptr >> 8; f[3] = ptr;
    }
  }
  file->Destroy();
  REXLOG_INFO("[xex] XexLoadImageHeaders({}, taille {}) -> 0x{:08X}", path, size, status);
  ctx.r3.u64 = status;
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

// NicGetLinkState(interface, r4) : etat du lien reseau. Une console hors ligne a un lien DOWN.
// Le HUD de XAM interroge cette fonction en boucle pendant son initialisation : le stub renvoyait
// r3 inchange (valeur arbitraire), donc un etat aleatoire. On repond explicitement « pas de lien »,
// ce qui est la verite sur une machine sans Xbox LIVE et evite toute attente.
// NE PAS ECRIRE A TRAVERS r4 : l'appelant reseau (sub_81910F30) l'appelle avec le r4 HERITE du
// contexte (sub_819231E8 ne charge que r3) et n'utilise que r3 — ecrire la page pointee a
// segfaulte au boot quand elle n'etait pas commitee (banc 2026-11-21). Les appelants qui lisent
// *r4 l'ont pre-rempli a zero : 0 = lien absent, le meme etat que r3.
REX_HOOK_RAW(__imp__NicGetLinkState) {
  (void)base;
  if (++g_nic_calls == 1)
    REXLOG_INFO("[net] NicGetLinkState : pas de lien (hors ligne)");
  ctx.r3.u64 = 0;
}

// NicGetStats(-1, r4) : compteurs du NIC (octets/paquets emis et recus). r3 = -1 (« tous les NIC »),
// r4 = tampon de 72 octets (6 x u64 puis 6 x u32, etendue lue par l'appelant). Retour ignore.
// Appelants : xam/dash_recomp.28.cpp:8849/8880/8911/8945 (sub_8190ED78 : r4 = r1+80, etend les
// valeurs en sommes de compteurs), 14.cpp:27710 (sub_81AE8270 : r4 = objet reseau +13076, la somme
// des 4 u64 est comparee a 0 : > 0 sinon la branche est sautee) et 35.cpp:27961 (sub_81AE8718 :
// r4 = r1+96, la division par un compteur est protegee). Raison : le stub laissait r3 et le tampon
// tels quels (residu de pile / objet), donc des compteurs arbitraires ; une console sans NIC actif
// n'a aucun trafic : on remplit 72 octets de zeros (pas de division par zero ni de boucle : les
// sommes et la comparaison > 0 ont ete lues). Les tampons sont lus par l'appelant juste apres,
// donc deja commits (contrairement a NicGetLinkState). Retourne 0.
REX_HOOK_RAW(__imp__NicGetStats) {
  if (ctx.r4.u32 && ctx.r4.u32 < 0xF0000000u) std::memset(base + ctx.r4.u32, 0, 72);
  if (++g_nicstats_calls == 1)
    REXLOG_INFO("[net] NicGetStats : compteurs a zero (aucun trafic)");
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
  // NECESSAIRE : ne PAS renvoyer STATUS_DLL_NOT_FOUND (0xC0000135) ici. Mesure (bissection) : ce
  // code remonte jusqu'a sub_8187A038, y echoue la comparaison aux statuts toleres (0xC0050005 /
  // 0xC005000F) et declenche VdDisplayFatalError(0x00012247) — erreur fatale d'affichage, ecran
  // noir. Un échec « honnête » ici coute donc l'affichage entier.
  ctx.r3.u64 = 0;
}

// HalGetPowerUpCause(&etat) : pourquoi la console a demarre (bouton, eject, reveil reseau...).
// Remplit une structure que XAM lit ensuite octet par octet.
//
// Pourquoi c'est important : juste apres l'appel, XAM lit le bit 0 (bit de poids faible) de l'octet a l'offset 4
// (sub_81892A60 : lbz r11,4(r11) puis clrlwi r3,r11,31) et le renvoie. Son appelant (sub_81872230) appelle
// VdDisplayFatalError (« erreur fatale d'affichage ») si ce bit est a 1 et saute l'erreur s'il est a 0 (polarite
// detaillee dans le corps ci-dessous) : le bit doit donc etre a 0.
//
// Le stub du SDK ne remplissait rien. On remplit ici toute la structure : demarrage normal (mise sous tension par
// l'utilisateur, ce qui est la verite sur un PC), bit 0 de l'octet 4 a 0.
REX_HOOK_RAW(__imp__HalGetPowerUpCause) {
  const uint32_t out = ctx.r3.u32;
  if (out && out < 0xF0000000u) {
    // Structure de 8 octets remise a zero, puis :
    //   octet 0    : type de mise sous tension = 1 (bouton / mise sous tension normale)
    //   octet 4    : drapeaux ; le bit 0 doit etre a 0 pour que XAM ne signale pas d'erreur
    //                d'affichage (voir sub_81892A60 et sub_81872230 dans le code recompilé).
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

// VdSwap : appel par lequel le code invite presente une image. Sans lui, le presentateur natif ne
// recoit jamais d'image — d'ou un ecran noir meme quand le HUD dessine. Observation seule : on note
// l'appel, puis on rend la main au SDK (qui fait le travail de presentation).
REX_HOOK_RAW(__imp__VdSwap) {
  // On COMPTE, puis on laisse le SDK presenter : le remplacer sans l'appeler supprimerait
  // l'affichage au lieu de l'observer.
  static PPCFunc* sdk = dash::SdkFunction("__imp__VdSwap");
  if (++g_vdswap_calls <= 6)
    REXLOG_INFO("[swap] VdSwap appele (r3=0x{:08X}) lr=0x{:08X}", ctx.r3.u32, uint32_t(ctx.lr));
  if (sdk) sdk(ctx, base);
}

// XInputdGetLastTextInputTime(index_manette, uint64_t* sortie) : appelant xam/dash_recomp.38.cpp:4780
// (sub_818C596C), boucle sur 4 manettes ; r3 != 0 = « pas de donnee », la manette est sautee puis
// le code retombe sur HidGetLastInputTime. Aucune saisie texte : on renvoie ERROR_DEVICE_NOT_CONNECTED
// (1167) sans toucher a la sortie.
REX_HOOK_RAW(__imp__XInputdGetLastTextInputTime) {
  (void)base;
  if (++g_textinput_calls == 1)
    REXLOG_INFO("[xinput] XInputdGetLastTextInputTime : aucune saisie (1167)");
  ctx.r3.u64 = 1167;
}

// UsbdGetNatalHardwareVersion() : version du materiel Kinect (1 ou 2 = present). Appelants :
// xam/dash_recomp.9.cpp:4954, 20.cpp:16413, 46.cpp:17083, 51.cpp:16903 ; toute valeur autre que 1/2
// prend le chemin « pas de Kinect » (erreur 0x8007048F pour le premier). On renvoie 0 : aucun Kinect.
REX_HOOK_RAW(__imp__UsbdGetNatalHardwareVersion) {
  (void)base;
  if (++g_natal_calls == 1)
    REXLOG_INFO("[usbd] UsbdGetNatalHardwareVersion : 0 (pas de Kinect)");
  ctx.r3.u64 = 0;
}

// XamGetLiveHiveValueW(nom, tampon, taille, -1, 0) : valeur de la « ruche » Xbox LIVE. Appelants :
// dash/dash_recomp.88.cpp:7543 (r3 < 0 = ignore le tampon, renvoie l'erreur) et dash_recomp.108.cpp:5739
// (r3 != 0 = chemin « absent »). Hors ligne : aucune valeur, ERROR_NOT_FOUND (0x80070490, negatif).
REX_HOOK_RAW(__imp__XamGetLiveHiveValueW) {
  (void)base;
  if (++g_hive_calls == 1)
    REXLOG_INFO("[live] XamGetLiveHiveValueW : valeur absente (hors ligne)");
  ctx.r3.u64 = 0x80070490u;
}

// XInputdGetDeviceStats(index, sortie) : appelants xam/dash_recomp.7.cpp:4646 et 24.cpp:4721 ; r3 < 0
// donne l'erreur 1167 (manette absente), sortie non lue. Aucune manette : echec negatif, sortie intacte.
REX_HOOK_RAW(__imp__XInputdGetDeviceStats) {
  (void)base;
  if (++g_devstats_calls == 1)
    REXLOG_INFO("[xinput] XInputdGetDeviceStats : manette absente (0x8007048F)");
  ctx.r3.u64 = 0x8007048Fu;
}

// EtxConsumerProcessLogs(...) : appelant xam/dash_recomp.37.cpp:31254 ; r3 >= 0 = succes, sinon
// seuls 0x80000011 / 0xC0000225 sont tolerés. Aucun journal ETW a traiter : succes (comme
// EtxProducerRegister / EtxConsumerRegister).
REX_HOOK_RAW(__imp__EtxConsumerProcessLogs) {
  (void)base;
  if (++g_etxlogs_calls == 1)
    REXLOG_INFO("[etx] EtxConsumerProcessLogs : succes (rien a traiter)");
  ctx.r3.u64 = 0;
}
