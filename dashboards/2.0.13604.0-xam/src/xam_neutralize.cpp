// Accroches des fonctions de XAM recompilé dont le démarrage est neutralisé (palier G).
//
// Chaque fonction est déclarée dans xam_overrides.toml par une entrée [[midasm_hook]] placée à sa première
// instruction avec `return = true` : le code généré appelle la fonction ci-dessous puis revient sans exécuter le
// corps d'origine. Les cibles sont des services inutiles au Dashboard ; aucune clé ni protection n'est contournée.

#include <rex/logging.h>
#include <rex/ppc/context.h>

// sub_818CD070 : ouvre \Device\DeviceAuth (authentification des périphériques de stockage), absent ici.
void xam_skip_device_auth(PPCRegister& r3) {
  REXLOG_INFO("[xam] sub_818CD070 (écoute DeviceAuth) neutralisée");
  r3.u64 = 0;
}
