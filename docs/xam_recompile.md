# XAM 13604 recompilé dans le processus du Dashboard (palier A)

Suite de « Dashboard 2.0.17559 et XAM recompilé » (README, phase P4). Objectif du palier : `dashboards/2.0.13604.0-xam/`
démarre avec `xam.xex` 13604 recompilé chargé comme module, le hub s'affiche, les threads système de XAM vivent.

## État

| Étape | État | Preuve |
|---|---|---|
| 1. Images décodées | fait | `work/xam13604.pe` reproduit à l'identique par `tools/xexinfo` (MD5 98d71f82...) ; `work/xam13604_rekey.xex` reproduit par `tools/xex_rekey.py` depuis le `$flash_xam.xex` de la mise à jour 13604 (`work/port/2.0.13604.0/extract/su20076000_00000000/`). |
| 2. Codegen de XAM en module | fait | `tools/codegen_module.py` (projet de travail `work/xam13604proj/`, journal `codegen_module.log`) : 16 914 fonctions connues, 9 cibles non résolues, 83 fonctions par pointeur, 126 fichiers, 61 Mo. |
| 3. Projet `dashboards/2.0.13604.0-xam/` | fait (compile) | codegen `rexglue` : 390 fichiers ; `libdash_xam.so` (33 Mo) et `dash` compilés. |
| 4. Démarrage et blocage | partiel | voir ci-dessous. |
| 5. Critère de fin | partiel | (a) hub : non atteint avec XAM chargé ; (b) threads système : atteints, sans `FATAL` pendant 90 s ; (c) liste ci-dessous. |

### Étape 4 : ce qui a été corrigé

Les trois premiers essais tombaient sur le même endroit (threads système de XAM). Chaque correctif est dans `common/` ou dans
le projet, sans toucher au SDK.

1. **Ordre de montage** (`common/src/dash_app_base.cpp`) : la flash (`\SystemRoot`, `\Device\Flash`) était montée
   après le chargement de XAM. Elle est désormais montée avant (`dash::xamrt::MountFlash`). Pour `DASH_XAM_MODULE`,
   `\Device\Harddisk0\Partition1` et `\Device\MU0` sont montés sur `DASH_XAM_STATE` (défaut
   `${XDG_DATA_HOME:-~/.local/share}/xenondash/xam`), `common/src/dash_xam_runtime.{h,cpp}`. Les erreurs
   `\Device\Flash\sutrace.debug ... device not found` disparaissent. Insuffisant seul (crash SIGSEGV toujours là).
2. **Processus système** (`dash::xamrt::RunInSystemProcess`) : `KeGetCurrentProcessType()` renvoie le type du processus
   du thread. Le point d'entrée de XAM teste `== 2` (processus système) avant d'utiliser son état (`sub_8188FAB8`,
   `KeTlsGetValue` sur l'index écrit par `sub_81891918`). `rex::system::XHostThread` place son thread dans le processus
   titre (type 1), donc le repli renvoie un objet nul et `RtlEnterCriticalSection` plante. Le thread « XAM load » est
   désormais un `XThread` créé avec `ks->GetSystemProcess()`. Le crash disparaît.
3. **Démontage** (`common/src/kernel_stubs.cpp`) : `IoDismountVolumeByName` renvoyait une valeur non nulle (stub du SDK) ;
   la machine d'état du stockage de XAM le rappelait sans fin (8,3 millions de lignes en 90 s). Surcharge : `STATUS_SUCCESS`.
4. **Enregistrement du module** (`dashboards/2.0.13604.0-xam/CMakeLists.txt`) : voir « Régression du SDK » ci-dessous.

### Régression du SDK (fork `third_party/rexglue-sdk`)

Le commit `e28335b` (8 novembre) a retiré `{{ project }}_register.cpp` du gabarit `resources/templates/codegen/sources_cmake.inja`
pour **tous** les projets. Pour une DLL module, `generated/xam/dash_register.cpp` (qui définit `ReXModule_Register` et
`ReXModule_GetImageInfo`) n'est donc plus compilé : le chargement échoue avec « ReXModule_Register not found in 'dash_xam' ».
Le 17559-xam, généré le 6 novembre, listait encore ce fichier.

- Contournement dans le projet (`CMakeLists.txt` du 13604-xam) : `target_sources(dash_xam PRIVATE generated/xam/dash_register.cpp)`
  après `include(generated/rexglue.cmake)`. À retirer quand le SDK le rétablit.
- Correctif SDK (non appliqué) : dans `sources_cmake.inja`, réinsérer `{{ cmake_var("CMAKE_CURRENT_LIST_DIR") }}/{{ project }}_register.cpp`
  quand `is_dll` est vrai.
- À vérifier : le 17559-xam régénéré avec ce SDK aura le même problème.

### Mesures (étape 4, runs sous Xvfb `:92` + lavapipe, captures et journaux hors dépôt)

| Run | Réglage | Résultat |
|---|---|---|
| `m2a` | `DASH_XAM_MODULE=2`, module introuvable | hub affiché (`m2a.png`), `game:\xam.xex` absent |
| `m2c` | module trouvé, thread hôte standard | SIGSEGV (139) à 33 s ; gdb : `RtlEnterCriticalSection_entry` appelé depuis `__imp__sub_818EE808` (`dash_recomp.29.cpp:6568`), pile : `sub_818BA678` (43.cpp:4197) ← `sub_818BA778` (23.cpp:4359) ← `sub_818BB140` (30.cpp:4387) ← `sub_8187AD20` (55.cpp:1537) ← `sub_81872460` (24.cpp:1389) ← `xstart` (53.cpp:1271) |
| `m2d` | + devices | même SIGSEGV à 36 s |
| `m2e` | + thread processus système | vivant à 90 s ; 9 threads système ; 8,3 M de lignes `IoDismountVolumeByName - stub` |
| `m2f` | + démontage `STATUS_SUCCESS` | vivant à 90 s ; 212 lignes ; **écran noir** (`m2f.png`) |
| `base2` | sans module (même binaire) | **hub affiché** (`base2.png`) |
| `m1a` | `DASH_XAM_MODULE=1` (chargé, sans point d'entrée) | `FATAL` à 20 ms du chargement : appel à `0xFFFFFFFF` depuis `dash_recomp.35.cpp:844` (`bctrl`, lr `0x9219F084`) |

Les fichiers XAM absents des périphériques factices sont journalisés en erreur sans arrêter XAM : `secexp.xex`
(`\Device\Harddisk0\Partition1`), `xstudio.xex` (`\Device\Flash`), `systemupdate.xex` (`\Device\Mass0`).

Gdb en batch : `gdb -batch` lancé avec le Dashboard puis SIGINT envoyé à gdb après 60 s (l'attache par `ptrace` est
interdite par `ptrace_scope=1`). Pile à 60 s avec module : le thread principal du Dashboard est dans `ExCreateThread_entry`
(destruction d'un `XThread`) ; les threads système de XAM dorment (`nanosleep`, `KeWaitForMultipleObjects`).

## Blocages restants, classés

1. **Écran noir avec XAM chargé** (`m2f`, `m2h`) alors que le hub s'affiche sans module (`base2`). Cause du `FATAL`
   (`m1a`) établie ; cause du hub noir probable. Voir « Palier B ».
2. **Instructions non implémentées dans XAM** : traitées par `tools/xam_unimpl_patch.py` (voir « Palier B »).
3. **Redirection des imports** non commencée : le Dashboard appelle toujours le HLE (`__imp__Xam*`, `__imp__Xui*`). Le
   module tourne à côté sans rien servir au Dashboard. Plan : générateur `REX_HOOK_RAW` par ordinal (README, palier 3).
4. **Contenu des périphériques** : les fichiers attendus par XAM (`secexp.xex`, `xstudio.xex`, `systemupdate.xex`) ne sont
   pas dans les répertoires d'état, qui sont vides au départ.
5. **Variables et stubs noyau** : `xam:0x3db` (`g_XuiAutomation`), `xboxkrnl:0x157` (`XboxKrnlBaseVersion`) non
   implémentées ; `EtxProducerRegister`, `KeSetPriorityClassThread`, `Drv*Callback`, `XeKeysGetStatus`, `NicGetLinkState`
   en stub ; `set_priority: permission denied` (3 fois).
6. **SDK** : régression du gabarit de registre (ci-dessus).

## Reproduire

```sh
# 1. image décodée (work/, non versionnée)
tools/xexinfo/build/xexinfo work/xam13604_rekey.xex work/xam13604.pe
# 2. codegen module (projet de travail dans work/, manifeste xam13604_manifest.toml)
python3 tools/codegen_module.py work/xam13604proj <manifeste> xam work/xam13604.pe \
  817E0000 81850000 383400 817E0400:8182DADC 81BE0000:81CBA840
cp work/xam13604proj/xam_auto_overrides.toml work/xam13604proj/xam_overrides.toml dashboards/2.0.13604.0-xam/
# 3. flash : xam.xex et lien dans assets/ du 13604 (fichiers Microsoft, ignorés par git)
cp work/xam13604_rekey.xex dashboards/2.0.13604.0/assets/flash/xam.xex
ln -s flash/xam.xex dashboards/2.0.13604.0/assets/xam.xex
# 4. projet
cd dashboards/2.0.13604.0-xam && ../../sdk/bin/rexglue codegen dash_manifest.toml
python3 ../../tools/xam_unimpl_patch.py generated/xam   # crmove, crnot, vavguw (palier B)
cmake --preset linux-amd64-relwithdebinfo && cmake --build out/build/linux-amd64-relwithdebinfo -j 6
# 5. lien du plugin GPU (manquant sinon : « GPU plugin 'xenos' not found »)
ln -sf "$PWD/../../sdk/lib/librexgpu-xenos.so" out/build/linux-amd64-relwithdebinfo/librexgpu-xenos.so
# 6. lancement (écran et captures choisis par variables)
DASH_DISPLAY=92 DASH_RUNS=<dossier_hors_depot> DASH_XAM_MODULE=2 DASH_ENV="DASH_XAM_STATE=<dossier> ..." ./run_headless.sh 90 essai
```

Contraintes : `assets` du projet 13604-xam est un lien de répertoire (le SDK refuse les liens par fichier hors de la racine
du jeu) ; `game:\xam.xex` est lu depuis `assets/xam.xex`.

## Fichiers

- Nouveaux : `dashboards/2.0.13604.0-xam/` (CMakeLists.txt, CMakePresets.json, dash_manifest.toml, xam_overrides.toml,
  xam_auto_overrides.toml, run_headless.sh, input_test.sh, src/), `common/src/dash_xam_runtime.{h,cpp}`,
  `tools/xam_unimpl_patch.py`, ce document.
- Modifiés : `common/src/dash_app_base.cpp`, `common/src/kernel_stubs.cpp`, `common/CMakeLists.txt`.
- Ignorés par git (assets Microsoft, build) : `dashboards/2.0.13604.0/assets/flash/xam.xex`, `dashboards/2.0.13604.0/assets/xam.xex`,
  `work/xam13604*`, `work/xam13604proj/`.
- Compilation vérifiée : `dashboards/2.0.13604.0` (HLE, rc=0 après les changements de `common/`) et `dashboards/2.0.13604.0-xam`.
  Le 17559 et le 6770 n'ont pas été recompilés.

## Palier B : cause du hub noir et correctifs

### Cause établie : tas 0x8 et 0x9 superposés, table de fonctions de XAM

- Les tas `0x80000000` et `0x90000000` sont deux vues du même fichier partagé (`/proc/PID/maps` : hôte `0x180000000` et
  `0x190000000` ont le même décalage fichier `0x80000000`). L'octet invité `0x92000D64` (dash.xex) est donc le même que
  `0x82000D64`.
- La table de fonctions de XAM est placée à `image_base + image_size` = `0x81D50000`, de taille
  `2 × (code_size + 0x10000)` = `0x727098` : elle couvre `0x82000000-0x82477098`, soit le début de dash.xex en mémoire
  physique. `Memory::InitializeFunctionTable` la remet à zéro (`Zero`), puis `SetFunction` y écrit des pointeurs de
  fonctions hôte (8 octets par fonction).
- Preuves (gdb en batch, `probe8.py`, watchpoint sur l'alias `0x182000D64`) : écrivain 1 = `memset` dans
  `Memory::InitializeFunctionTable` (la case passe à 0) ; écrivain 2 = `Memory::SetFunction` appelé par
  `ReXModule_Register` (la case passe à `FE7F0000`).
- La case `0x92000D64` est un enregistrement d'import du Dashboard : `SetupLibraryImports` y écrit `0x30002000`
  (variable de noyau), puis la table de XAM la détruit. Le `FATAL` de `DASH_XAM_MODULE=1` vient de l'appel de
  journalisation `sub_9219F010` (`dash_recomp.35.cpp:780`), qui lit `[[0x92000D64]] + 24` ; la case vaut
  `0xFFFFFFFF`, un pointeur hôte écrit par `SetFunction`.
- Un watchpoint sur `0x192000D64` seul ne voit rien : les écritures passent par l'alias `0x182000D64`.
- Le hub noir avec `DASH_XAM_MODULE=2` relève du même mécanisme (les 4,6 Mo de début de dash.xex sont écrasés). Non vérifié
  isolément.

### Essai de déplacement de la table (SDK, annulé)

- Le déplacement de la table hors de l'alias (contrôle des pages des deux tas et des recouvrements entre tables, table
  placée en `0x83E10000`) fonctionne au chargement, mais le code généré ne lit pas la table là où elle est déplacée :
  `REX_LOOKUP_FUNC` (`generated/xam/dash_pch.h`) calcule `REX_IMAGE_BASE + REX_IMAGE_SIZE` en dur. Un appel indirect lit
  alors les données de dash.xex : crash SIGSEGV à 49 s, dans la boucle d'initialisation de XAM (`sub_818FCDB0`,
  `call *%rax`, cible `0x3e80002918b`).
- Les réservations de pages ne suffisent pas : `BaseHeap::Reset` (`xex_module.cpp:526`) efface la table de pages du tas
  à chaque chargement de XEX (la réservation de la table du Dashboard disparaît au chargement de dash.xex) ;
  `AllocFixed` avec `Reserve|Commit` ne détecte pas les recouvrements ; les tas `0x8` ont des pages de 64 Ko, les tas `0x9`
  de 4 Ko.
- Le SDK est restauré à son état d'origine (`sdk/lib/librexruntime.so`, md5 `067250b8…`). Le diff de l'essai est gardé hors
  dépôt.

### Correctif (appliqué : voir « Palier C »)

1. Codegen : remplacer `REX_IMAGE_BASE + REX_IMAGE_SIZE` de `REX_LOOKUP_FUNC` par `REX_TABLE_BASE`, émis par module
   (`resources/templates/codegen/`), et ajouter `table_base` à `PPCImageInfo` (valeur par défaut : fin d'image, donc
   compatible avec le Dashboard HLE et le 17559).
2. `Memory::InitializeFunctionTable` : accepter `table_base`, vérifier le recouvrement en adresses physiques (bit 28)
   avec les tables existantes et les images, réserver l'alias quand la page le permet, et ne pas se fier à la table de
   pages seule (`Reset`).
3. Régénérer `dashboards/2.0.13604.0-xam` avec un `table_base` libre physiquement (par exemple `0x84000000`).
4. À vérifier ensuite : la plage alias de l'image XAM (`0x917E0000-0x91D50000`) n'est pas réservée dans le tas `0x9`. Le
   Dashboard peut y allouer ; non vérifié.

### Instructions non implémentées (fait)

- `crmove` (0x81936D14), `crnot` (0x81936D28) et `vavguw` (0x81B19A48) : `tools/xam_unimpl_patch.py <generated/xam>` les
  remplace dans le code généré par du C++ direct (`ctx.cr1.so = ctx.cr7.so;`, `ctx.cr0.eq = !ctx.cr0.eq;`, moyenne par
  élément). Le SDK de la révision courante ne les génère pas. À relancer après chaque `rexglue codegen` du projet XAM.
  Vérifié : compilation de `libdash_xam.so`, et aucun `Unimplemented PPC` en 90 s (ces chemins ne sont pas atteints
  pendant les essais).

### Critères du palier B

| Critère | État |
|---|---|
| Hub affiché avec `DASH_XAM_MODULE=2` | non atteint : écran noir (`m2h`) |
| Pas de `FATAL`, threads système vivants, 60 s stables | atteint pour `m2h` (vivant 90 s, aucun `FATAL`) |
| Instructions manquantes | fait (`tools/xam_unimpl_patch.py`) |
| Redirection d'un import vers XAM recompilé | non fait (bloqué : le hub doit d'abord être corrigé) |

## Palier C : table de fonctions relocalisable (SDK)

Le recouvrement de la table de XAM avec dash.xex est corrigé. Le hub reste noir : voir « Résultat » ci-dessous.

### Correctif (`third_party/rexglue-sdk`, non commité)

- **Codegen** : clé optionnelle `function_table_base` (TOML). Placée au niveau racine d'un fichier de configuration, ou dans
  l'entrée `[[modules]]` du manifeste (lue par `LoadFromTable`). Défaut : fin de l'image (`IMAGE_BASE + IMAGE_SIZE`).
  Les gabarits n'émettent `REX_TABLE_BASE` et `.table_base` que si la clé est présente.
- **Sortie inchangée sans la clé** : Dashboard 2.0.13604.0 et 2.0.6770.0 régénérés avec `--ignore-stamp` ; aucun fichier
  source ne change (seul `codegen.stamp` change, il contient la version du SDK). Le projet XAM a `REX_TABLE_BASE` (2 occurrences).
- **Runtime** : `PPCImageInfo::table_base` (ajouté en fin de structure). `Memory::InitializeFunctionTable(..., table_base)` et
  `FunctionDispatcher::InitializeFunctionTable(..., table_base = 0, is_entrypoint = false)`. Les appelants passent la valeur
  (`runtime.cpp`, `kernel_state.cpp`).
- **Recouvrement** : les tas `0x8` et `0x9` sont deux vues du même fichier (bit 28 non significatif). `GuestRangesOverlap`
  (`include/rex/system/xmemory.h`) compare donc en adresses physiques. Le dispatcher contrôle image, table et code de chaque
  module enregistré ; la mémoire contrôle les tables existantes.
- **Pages** : `AllocFixed` ne refuse une plage occupée que pour une réservation seule (`Reserve`). Avec `Reserve|Commit`, le
  contrôle n'existe pas. La table est donc réservée seule (refus si occupée), puis validée. Son alias dans l'autre tas est
  réservé de la même façon et libéré à la destruction du module.
- **Image de XAM** : `ReserveImageAlias` (`src/system/kernel_state.cpp`) réserve l'alias de l'image chargée dans l'autre tas
  (`0x917E0000-0x91D50000` pour XAM). Refuse le chargement si une page est déjà prise.

### Projet XAM

- `dash_manifest.toml`, entrée `[[modules]]` de XAM : `function_table_base = 0x84000000`.
- Plages (adresses physiques, bit 28 retiré) : dash.xex `0x92000000-0x92EA8000` (tas `0x9`, pages de 4 Ko), table du Dashboard
  `0x92EA8000-0x93E05B10`, image de XAM `0x817E0000-0x81D50000`, table de XAM `0x84000000-0x84727098` (alias `0x94000000`).
- Journal du chargement : `Function table initialized for module: code=81850000-81BD384C, image=817E0000-81D50000, table=84000000`.

### Tests et compilation

- `tests/unit/system/function_dispatcher_test.cpp` : 12/12 cas `[dispatcher]` (4 nouveaux : table relocalisée, défaut
  inchangé, recouvrements direct, par alias et hors tas refusés, alias déjà occupé). Suite complète : 212 cas sur 221 réussis ;
  les 5 échecs portent sur BitStream, le calendrier et Depfile (modules sans lien ; non vérifiés sur la base d'origine).
- Compilés (rc 0) : `dashboards/2.0.13604.0-xam`, `dashboards/2.0.13604.0`, `dashboards/2.0.6770.0`.

### Résultat (`DASH_XAM_MODULE=2`, environnement de référence : `VK_ICD_FILENAMES` lavapipe, `HOME` et `XDG_DATA_HOME` de test)

| Run | Réglage | Résultat |
|---|---|---|
| `s5base2` | sans module | **hub affiché** à 60 s, rendu continu |
| `s5c`, `s5e` | module 2, table 0x84000000 | vivant 90 s (`s5c`) et 60 s ; aucun `FATAL`, aucun SIGSEGV ; **écran noir** |
| `s5a` | module 2, sans l'environnement de référence | SIGSEGV à 20 s (une fois) ; pile : lambda `XThread::Create()::$_0`, décalage `0x4bb581` de `librexruntime.so` ; non reproduit en 3 runs de référence |

Constats sur le run module (le journal est complet avec `DASH_ARGS=--log_flush_interval=1`, sans ce réglage il est tronqué) :

- Le journal s'arrête 0,3 s après le chargement de XAM (`xam.xex recompilé : chargé`). Sans module, le rendu produit des
  lignes `[gpu]` en continu. Avec module, aucune.
- À ce moment, neuf threads système sont créés (`[ExCreateThread] Guest is creating a system thread!`, aucun dans le run de référence).
- `gdb` à 60 s (`gdb/stall60.out`, méthode `interrupt.sh`) : le thread principal du Dashboard est dans `ExCreateThread_entry`,
  en destruction d'un `XThread` ; les threads de XAM attendent (`KeWaitForSingleObject`).
- Hypothèse « le Dashboard alloue dans l'alias de l'image XAM » (blocage n° 2 ci-dessous) : **réservation réussie** au
  chargement (`Module image alias 917E0000-91D50000 ... reserved`), le blocage persiste. Cette cause n'explique donc pas
  le hub noir à elle seule.
- Import du Dashboard : aucun `FATAL` (le `m1a` provenait de l'écrasement de `0x92000D64`) ; la table de XAM ne le touche plus.

### Blocages restants

1. **Hub noir avec XAM chargé** : le Dashboard cesse de produire du rendu peu après le chargement de XAM. Cause non établie.
   Pistes : la boucle de création de threads du thread principal (dont la destruction de `XThread` observée à 60 s) ; les
   9 threads système créés au chargement ; le `Reset` du tas `0x8` au chargement d'un XEX, qui efface les réservations
   (y compris celle de l'alias de la table du Dashboard).
2. **Plantage isolé** (`s5a`) : SIGSEGV dans le lambda de démarrage des `XThread` (`librexruntime.so+0x4bb581`), non reproduit
   dans l'environnement de référence. Core dump géré par `systemd-coredump` (`/var/lib/systemd/coredump`, lisible par root seulement).
3. **Redirection des imports** : non faite (bonus, bloquée par le point 1).
4. Les réservations d'alias ne sont ni sauvegardées (`Save`/`Restore`) ni libérées au déchargement d'un module ; à vérifier.

### Procédure (après modification du SDK)

```sh
cd third_party/rexglue-sdk && cmake --build out/build/linux-amd64 --config Release --target install -j 6
cd dashboards/2.0.13604.0-xam && ../../sdk/bin/rexglue codegen --ignore-stamp dash_manifest.toml
python3 ../../tools/xam_unimpl_patch.py generated/xam
cmake --preset linux-amd64-relwithdebinfo && cmake --build out/build/linux-amd64-relwithdebinfo -j 6
DASH_DISPLAY=92 DASH_RUNS=<dossier_hors_depot> DASH_XAM_MODULE=2 DASH_ARGS="--log_flush_interval=1" \
  DASH_ENV="HOME=<test> XDG_DATA_HOME=<test> VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json DASH_XAM_STATE=<test>/xam" \
  ./run_headless.sh 90 essai
```

Le correctif `tools/xam_unimpl_patch.py` reste à relancer après chaque `rexglue codegen` du projet XAM.
