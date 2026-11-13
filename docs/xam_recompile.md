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

## Palier D : le hub s'affiche avec XAM chargé

Résultat : le hub NXE s'affiche avec `DASH_XAM_MODULE=2` (captures hors dépôt : `runs/f1.png` à 60 s, `runs/f2.png` à 100 s).
Le processus est vivant, les 9 threads système de XAM sont créés, aucun `FATAL`, aucun SIGSEGV.

### Cause du hub noir (établie)

Chaîne complète, observée par piles gdb et journal de diagnostic (instrumentation temporaire du SDK, retirée) :

1. XAM (fonction `sub_81883CB8`, `generated/xam/dash_recomp.38.cpp:2238`, appelée depuis un thread système) appelle
   `ObReferenceObjectByName(&nom_ansi, ..., &objet)` sur un nom de périphérique, puis `ObDereferenceObject(objet)` sans
   contrôler le statut.
2. Le SDK ne lit pas `nom` comme un `ANSI_STRING` : `ObReferenceObjectByName_entry` (`src/kernel/xboxkrnl/xboxkrnl_ob.cpp`)
   prend l'adresse de la structure pour une chaîne C. Journal : `ObReferenceObjectByName '' -> 0xc0000034` (nom vide,
   recherche toujours en échec). Le SDK n'écrit pas `*objet` en cas d'échec.
3. `*objet` garde donc une valeur ancienne : `0xBEBEBEBE` (boucle de XAM, sans fin) ou le pointeur d'un objet précédent.
   `ObDereferenceObject` appelle alors `XObject::ReleaseHandle()` sur cet objet, qui décrémente le compte de handles d'un
   objet qui n'a pas été référencé.
4. Le Dashboard crée son thread : `XObject` ajoute le handle (compte 1, « référence du créateur »). Un thread de XAM
   déréférence ce handle (journal : `ReleaseHandle h=F8000114 hcnt=1 obj=<thread du Dashboard>`) : le compte tombe à 0,
   l'entrée de la table est retirée (`Release` du pointeur de la table).
5. `XThread::Create` puis `ExCreateThread_entry` se terminent : la dernière référence de pointeur est celle de
   `object_ref` (`ExCreateThread_entry`). Elle libère l'objet, `~XThread` (`ExCreateThread_entry` directement dans la pile).
6. Le thread hôte du nouveau XThread est suspendu (en attente de `Resume`) ; `~PosixThread` détruit la condition
   (`pthread_cond_destroy`) alors qu'un thread y est en attente : le thread principal du Dashboard (`Main XThread`) reste
   bloqué dans `pthread_cond_destroy` (pile gdb à 60 s). Plus de rendu, plus de journal.

Preuves retenues : pile du thread principal à 60 s (gdb, `Main XThread` dans `ExCreateThread_entry` → `~XThread` →
`~PosixThread` → `pthread_cond_destroy`) ; `XThread dtor` appelé directement depuis `ExCreateThread_entry` (gdb) ; deux
objets différents portant le même handle `F8000114` (le second créé par un thread de XAM après la suppression du
premier) ; piles des `ObDereferenceObject` sur `0xBEBEBEBE` (`sub_81883CB8` ← `sub_81890F18` ← thread système) ; sans
instrumentation, la même séquence (journal arrêté après « Created 9 graphics pipelines », process vivant).

### Correctif (`common/src/kernel_stubs.cpp`, SDK inchangé)

`REX_HOOK_RAW(__imp__ObReferenceObjectByName)` : nom lu comme `ANSI_STRING` (`TranslateAnsiStringAddress`) ; en cas
d'échec, `*objet = 0` et statut `STATUS_OBJECT_NAME_NOT_FOUND`. Sur succès, la référence est prise (`RetainHandle`), comme
dans le SDK. XAM voit alors `0` et ne déréférence pas (la branche `beq` de la fonction de XAM).

Ce correctif contourne le SDK. Le défaut à corriger dans le SDK (`xboxkrnl_ob.cpp`) :
- `ObReferenceObjectByName_entry` doit lire `X_ANSI_STRING*` (`util::TranslateAnsiStringAddress`), comme
  `LookupNamedObject` ;
- il doit écrire `NULL` dans `out_object_ptr` sur échec.
Le correctif de `common/` touche tous les projets qui lient `common/` (13604, 13604-xam, 6770 ; 17559 non recompilé). Il ne
couvre pas les objets de périphérique (`\Device\...`) : ils ne figurent pas dans la table des noms, la recherche échoue
toujours, mais sans déréférencement fautif.

### Vérifications

| Vérification | Résultat |
|---|---|
| `dashboards/2.0.13604.0-xam` (module 2) | hub affiché à 60 s et 100 s (`f1.png`, `f2.png`) ; 9 threads système ; 0 FATAL |
| `dashboards/2.0.13604.0` (HLE, même `common/`) | hub affiché à 40 s (`hle1.png`) |
| Compilation | `2.0.13604.0` et `2.0.6770.0` : rc 0 ; `2.0.13604.0-xam` : rc 0 |
| SDK | `third_party/rexglue-sdk` : aucun fichier modifié (instrumentation temporaire annulée par `git checkout`, `sdk/lib/librexruntime.so` md5 `769fc992…`, identique à l'état d'avant) |

### Blocages restants

1. **Threads de XAM en boucle** : après le correctif, deux threads système de XAM consomment 75 à 90 % d'un cœur
   (`Main XThread` à ~97 %, comme dans la référence sans module à ~98 %). Ils réessaient les recherches de périphériques
   (`\Device\...`, absents de la table des noms) : à traiter (objets de périphérique ou table de noms).
2. **Redirection des imports** (bonus) : non faite.
3. **Correctif SDK** : à faire dans `xboxkrnl_ob.cpp` (voir ci-dessus) ; le correctif de `common/` peut alors être retiré.
4. Sémantique de `ExCreateThread` (flags `0x80`, objet renvoyé sans référence) : non vérifiée.
5. Fenêtre de course : entre l'ajout du thread dans la table (`XObject`, compte 1) et le `RetainHandle` de `XThread::Create`,
   un déréférencement concurrent retire le thread de la table. C'est la fenêtre observée ; le SDK ne protège pas cette
   fenêtre.

## Palier E : redirection des imports du Dashboard vers XAM recompilé

Le Dashboard appelait encore le HLE du SDK pour chacun de ses imports XAM ; le module tournait à côté sans servir à rien.
Ce palier ajoute un mécanisme de redirection configurable, sans modifier le SDK.

### Étape 1 : mécanisme d'imports (constats)

- **Codegen** : un import du Dashboard devient un appel direct `__imp__<Nom>(ctx, base)`
  (`third_party/rexglue-sdk/src/codegen/builders/context.cpp`, `phase_register.cpp`). Le nom vient de la table d'exports du
  SDK (ordinal → nom).
- **Résolution à l'édition de liens** : `__imp__<Nom>` est fourni par le HLE du SDK (`REX_EXPORT` dans
  `src/kernel/xam/xam_ui.cpp` et voisins, dans `librexruntime.so`). Un `REX_HOOK_RAW(__imp__<Nom>)` dans l'exécutable
  l'emporte pour les appels de l'exécutable : c'est le mécanisme déjà utilisé par `common/`.
- **Ordinaux du Dashboard** : `work/dash_imports.tsv` (43 entrées `xam.xex`, dont l'ordinal 96, non nommé). Noms :
  `third_party/rexglue-sdk/src/kernel/xam/export_table.inc` (ex. ordinal 714 = `XamShowMessageBoxUI`).
- **Exports de XAM recompilé à l'exécution** : `XexModule::GetProcAddress(ordinal)` (`src/system/xex_module.cpp`) lit la
  table d'exports du XEX ; `UserModule::GetProcAddressByOrdinal` la relaie. La fonction recompilée se trouve ensuite dans
  la table de fonctions du module (`FunctionDispatcher::GetFunction`). Testé : XGetVideoMode (977) → `0x818CAD18`,
  XamShowMessageBoxUI (714) → `0x8189A3C0`, signature à 9 arguments conforme (le 9e est lu à `1412(r1)` après `stwu -1328`,
  soit `r1 + 0x54` de l'appelant).
- **Table d'exports PE de `work/xam13604.pe`** : son répertoire d'exports (RVA `0x4E0000`) pointe sur des zéros dans l'image
  décodée (données utiles décalées, en gros-boutiens) : non exploitable. Les noms viennent donc du SDK, les adresses de
  l'exécution.
- **Imports déjà remplacés dans `common/`** (non redirigeables sans toucher à leur fichier) : `XamShowAchievementsUI`
  (`dash_xam_ui.cpp`), `XamLoaderLaunchTitle` (`dash_launch.cpp`), `XamNotifyCreateListener` (`dash_disc.cpp`),
  `XamContentClose` et `XamContentCreateEnumerator` (`dash_library.cpp`, `dash_content.cpp`), `XamUserGetName`
  (`dash_profile.cpp`). Ils restent sur le hook de `common/`.

### Étape 2 : mécanisme (`common/src/dash_xam_redirect.{h,cpp}`)

- Un hook par import XAM du Dashboard (36 hooks, macro `DASH_XAM_REDIRECT_HOOK`). Chaque hook appelle `Dispatch` : si le
  nom figure dans `DASH_XAM_REDIRECT`, il exécute la fonction de XAM recompilé avec le même `ctx` ; sinon le HLE du SDK.
- `Configure` est appelée après `LoadUserModule("game:\\xam.xex")` (`dash_app_base.cpp`). Elle résout chaque nom en
  ordinal (table `xam.xex` du SDK), puis en adresse, et journalise `[xamredir] <nom> (ordinal N) -> XAM recompilé 0x...`.
- **Trace** : `[xamredir] <nom> : appel N par XAM recompilé|HLE` au premier appel de chaque import puis tous les 1000 appels.
- **Piège trouvé (interposition ELF)** : `librexruntime.so` exporte aussi `__imp__XGetLanguage` ; sa table de registre
  (`FindPPCFuncByName`) pointe sur le hook de l'exécutable, et le repli HLE s'appelait lui-même (récursion, SIGSEGV dans
  `Dispatch`). Sous Linux, le HLE est donc résolu par `dlsym(RTLD_NEXT)` ; sous Windows, la table de registre suffit (pas
  d'interposition entre DLL).
- Aucune modification du SDK. Sans `DASH_XAM_REDIRECT`, les imports passent tous par le HLE (même comportement qu'avant,
  seule la trace est ajoutée).

Utilisation :

```sh
DASH_XAM_MODULE=2 DASH_XAM_REDIRECT=XGetVideoMode,XamShowMessageBoxUI ./run_headless.sh 90 essai
DASH_TEST_MSGBOX=30   # test : appelle XamShowMessageBoxUI depuis un thread titre au bout de 30 s (appel asynchrone)
```

Imports appelés par le Dashboard au démarrage (trace, sans redirection) : XamLoaderGetLaunchData, XGetVideoMode,
XamUserGetXUID (1000 appels en 90 s), XamUserGetSigninState, XamEnumerate, XNotifyGetNext, XamInputGetKeystrokeEx,
XamInputGetState, XamLoaderGetDvdTrayState, XamContentCreateDeviceEnumerator. Les autres imports de la table n'apparaissent pas
dans ce démarrage.

### Étape 3 : premier test (XGetVideoMode)

| Run | Réglage | Résultat |
|---|---|---|
| `r5a` | `DASH_XAM_REDIRECT=XGetVideoMode` | journal : `XGetVideoMode (ordinal 977) -> XAM recompilé 0x818CAD18`, puis `appel 1 par XAM recompilé` ; vivant 90 s ; hub affiché (`runs/r5a.png`) |
| `r4a` | sans redirection | trace des imports appelés (liste ci-dessus), hub affiché |

### Étape 4 : XamShowMessageBoxUI via XAM recompilé (blocage classé)

Le test `DASH_TEST_MSGBOX` appelle la voie redirigée comme le ferait le Dashboard (hook → `Dispatch` → XAM). Résultats :

| Run | Arguments | Retour (r3) | Journal |
|---|---|---|---|
| `r6a` | 9e argument mal placé (`r1 + 0x28` au lieu de `r1 + 0x54`) : `overlapped` lu comme une valeur quelconque | `0x65B` (1627) | `appel 1 par XAM recompilé`, retour immédiat, résultat 0 |
| `r7a` | 9e argument à `r1 + 0x54`, `overlapped` nul | `0x57` (87, ERREUR_PARAMETRE) | aucun autre journal |
| `r8a` | `overlapped` = XOVERLAPPED à zéro (appel asynchrone) | `0x65B` (1627) | aucun autre journal ; capture hub affichée (`runs/r8a.png`) |

Lecture du code généré (`dashboards/2.0.13604.0-xam/generated/xam/dash_recomp.20.cpp`, fonction `sub_8189A3C0`) :

- Contrôles d'arguments (`loc_8189A400` à `loc_8189A484`) : utilisateur < 4 ou 255 ; drapeaux sans bits interdits ; nombre de
  boutons ; `overlapped` nul impose `pResult` nul (sinon 87). Avec les valeurs de test, ces contrôles passent.
- Corps : `sub_818FCD10`/`sub_818FCCA8` (initialisation de la structure), puis `sub_8189F0D0` (r3 non nul → chemin 254 avec
  `sub_8187B8E8` puis `r4 = 997` et `sub_81896B20`, qui renvoie r4 ; r3 nul → 5). Ce chemin devrait donner 997, 0 ou 5, pas
  0x65B : **la cause du 0x65B n'est pas établie**. Le contrôle `sub_8189F0D0` et les fonctions appelées n'ont pas été tracés
  (essai gdb : `handle SIGSEGV nostop noprint pass` requis, les points d'arrêt dans `sub_8189A3C0` n'ont pas été atteints).
- Aucun fichier XUI n'est demandé pendant l'appel : pas de `File not found` pour `hud.xex`, `huduiskin.xex`, `signin.xex`,
  `vk.xex`, `shrdres.xzp`. XAM refuse donc avant le chargement de l'interface.

Blocages classés (étape 4) :

1. **XamShowMessageBoxUI refuse avec 0x65B avant tout chargement de module XUI** (preuve : `r8a`). Cause non établie.
   Pistes : `sub_8189F0D0` (état de l'interface, à tracer) ; variable importée `xam:0x3db` (`g_XuiAutomation`) non
   implémentée, signalée à chaque chargement (`Variable import xam:0x3db (g_XuiAutomation) not implemented`), jamais
   renseignée. Une recherche des `li r3,1627` du code généré ne désigne pas un chemin évident.
2. **Chargement des modules XUI** (`hud.xex`, `huduiskin.xex`, `signin.xex`, `createprofile.xex`, `vk.xex`) : non observé
   (la voie n'y arrive pas). Prévision : ces XEX ne sont pas recompilés (aucun `[[modules]]` dans `dash_manifest.toml`) ;
   leur chargement par XAM ne donnerait que des images sans code. À confirmer une fois le point 1 levé.
3. **Périphériques absents** (existant) : `secexp.xex`, `xstudio.xex`, `systemupdate.xex` (`\Device\Mass0`) : journalisés,
   non bloquants pour le hub.
4. **Autres imports non testés** : seule XGetVideoMode est redirigée en route de production ; les entrées manette
   (`XamInputGetState`) et les profils (`XamUserGetSigninState`) restent sur le HLE par défaut, faute de test.

### Vérifications

| Vérification | Résultat |
|---|---|
| `dashboards/2.0.13604.0-xam` (module, redirection et test) | compile (rc 0), hub affiché (`r5a`, `r8a`), aucun SIGSEGV ni FATAL pendant 90 s |
| `dashboards/2.0.13604.0` (HLE) | compile (rc 0) : pas de symbole dupliqué avec les hooks de redirection |
| `dashboards/2.0.6770.0` | compile (rc 0) |
| SDK (`third_party/rexglue-sdk`) | aucun fichier modifié par ce palier |

### Fichiers

- Nouveaux : `common/src/dash_xam_redirect.h`, `common/src/dash_xam_redirect.cpp`.
- Modifiés : `common/CMakeLists.txt` (une source), `common/src/dash_app_base.cpp` (appel de `Configure` après le chargement de
  XAM), ce document.

### Reste à faire

- Lever le point 1 (0x65B) : tracer `sub_8189F0D0` et les fonctions de XAM appelées depuis `sub_8189A3C0`, avec gdb
  (`handle SIGSEGV nostop noprint pass`, points d'arrêt par nom dans `libdash_xam.so`) ; renseigner `xam:0x3db`.
- Recompiler les XEX XUI de la flash (`hud.xex`, `signin.xex`, ...) si XAM les charge (point 2).
- Étendre la liste des redirections (entrées manette, profils) après validation de chaque import.

## Palier F : boîte de message Xbox par XAM recompilé et `hud.xex` (jalon partiel)

Objectif : `XamShowMessageBoxUI` (redirigé vers XAM recompilé) affiche la vraie boîte de message de la console, fournie par
XAM et les modules XUI de la flash. **Non atteint** : l'appel est désormais accepté par XAM (retour `0x3E5`,
ERROR_IO_PENDING, au lieu de `0x65B`), mais aucune interface n'apparaît. La cause du refus et les neuf blocages levés
pour y arriver sont établis ci-dessous ; la suite est bloquée au démarrage d'une application de XAM (point 1 des
blocages restants). Aucun fichier du SDK n'est modifié : tous les correctifs sont dans `common/`.

### Étape 1 : cause du `0x65B` (établie)

Chaîne lue dans le code généré (`dashboards/2.0.13604.0-xam/generated/xam/`), confirmée par des traces temporaires
(`REXLOG` dans le code généré, retirées) et par la mémoire invitée lue au moment de l'appel (`DASH_TEST_PEEK`) :

1. `sub_8189A3C0` (XamShowMessageBoxUI) remplit une structure de 1148 octets et appelle `sub_8187B8E8` →
   `sub_8187B648(254, 0x2100D, ...)` : envoi du message `0x2100D` à l'**application XAM numéro 254** (0xFE).
2. `sub_81893EE0(254)` cherche l'entrée dans la table des applications (`0x81C06F78 - 184 * id`, 184 octets par entrée).
   Elle renvoie 0 si `entrée+8 == 0` ou `entrée+16 == 0`. `sub_8187B648` renvoie alors `0x80070057` (E_INVALIDARG).
3. `sub_81896B20` convertit tout HRESULT négatif en code Win32 : `14` pour `0x8007000E`, sinon **`1627` = `0x65B`**.
4. Mesure : à l'appel, les 255 entrées de la table étaient à zéro (`+8 == +16 == 0`). Les applications ne sont enregistrées
   que par `sub_818951C8` (`sub_818946D8`, table statique `0x817E4B60` : xlivebase 0xFC, uvbase 0xFD, xgi 0xFB, musicplayer 0xFA,
   Xam.LiveMessenger 0xF7, ximecore 0xF5, Xam.Community 0xF4, avatarcore 0xF2, avatarsyscore 0xF3, cert 0xF8), elle-même appelée
   par `sub_81872230` (initialisation de XAM) qui n'allait pas jusque-là.
5. Avec l'application 254 enregistrée (`+8 != 0`, `+16 == 1`, gestionnaire `sub_81894D50`), l'appel renvoie `0x3E5`. Le
   gestionnaire traite `0x2100D` par `sub_8189BE20` → `sub_818A21B0("hud.xex", ...)` (chaîne `0x817E4C3C`), qui renvoie
   `0x80004005` (E_FAIL) si l'état du HUD `0x81BFCAE8` (+172, +176) est nul. Ces champs sont posés par `sub_818A2C98`
   (`XexLoadImage("\SystemRoot\huduiskin.xex")`, `VdRegisterXamGraphicsNotification`, ...), planifiée par `sub_818A2F90`,
   appelée par `sub_81872230` **après** `sub_818951C8` et seulement si `XboxHardwareInfo.Flags & 0x200`.
   Mesure : les mots `0x81BFCAE8..0x81BFCBE8` sont nuls pendant tout le test (150 s).

Hypothèses de l'énoncé : (a) HUD chargé au démarrage de XAM : confirmé dans la forme (XAM tente `XexLoadImage("hud.xex")` au
démarrage : `VFS: 'hud.xex' -> [no device]`, puis `File not found: \Device\Harddisk0\Partition1\hud.xex`, et le HUD complet
passe par `sub_818A2C98`) ; (b) `xam:0x3db` (`g_XuiAutomation`) : non utilisée sur ce chemin ; (c) autre état d'initialisation :
**c'est la cause réelle** (initialisation de XAM interrompue).

### Étape 1 bis : blocages levés dans l'initialisation de XAM (tous dans `common/`)

Méthode : trace temporaire des appels directs de `sub_81872230` (`tr/trace_calls.py`, hors dépôt : ajoute une ligne `REXLOG`
avant et après chaque `sub_X(ctx, base)` d'une fonction choisie, avec r3/r4/r5), plus piles gdb (`SIGINT` à `gdb -batch`, `handle
SIGSEGV nostop noprint pass` : le Dashboard provoque des SIGSEGV normaux). Le fil d'initialisation s'arrêtait successivement à :

| # | Symptôme (preuve) | Cause | Correctif |
|---|---|---|---|
| 1 | `sub_81872230` bloque dans `sub_81879B58` (`KeWaitForSingleObject(*0x817E05C0 = 0x30000254)`, pile gdb) | `UsbdBootEnumerationDoneEvent` est créé par le SDK avec `type = 1`, soit `EventSynchronizationObject` (réarmement automatique) : le premier fil (`sub_81872180`) consomme le signal | `FixKernelVariables` (`dash_xam_runtime.cpp`) : type 0 (notification), comme sur la console |
| 2 | un fil de XAM à 100 % d'un cœur ; l'événement `0x81BE0DF8` jamais signalé | `DrvGetContentStorageNotification` est un stub qui laisse r3 inchangé ; `sub_8185C5E0` le rappelle tant que r3 >= 0 | `kernel_stubs.cpp` : renvoie `0xC0000001` (aucun périphérique USB) |
| 3 | le fil de répartition des tâches (`sub_81890DE8`) reçoit `0xC000000D` de `KeWaitForMultipleObjects` en boucle (2577 réveils en 30 s) ; les tâches de classe 1 (`sub_81890B38`) ne sont jamais exécutées, `sub_81859780` attend pour toujours | l'objet `0x81BFB6F0` est une minuterie invitée (en-tête type 9) : `XObject::GetNativeObject` refuse les types 8/9, et `KeSetTimerEx`/`KeCancelTimer` sont des stubs | `kernel_timers.cpp` : `KeInitializeTimerEx`, `KeSetTimerEx`, `KeCancelTimer` créent/pilotent un `XTimer` rattaché à l'en-tête invité (DPC non pris en charge, journalisé) |
| 4 | SIGSEGV aléatoire dans `AudioSystem::SubmitFrame` (fil audio, gdb) | XAM désinscrit son client audio pendant qu'un callback soumet une trame : `clients_[index].driver` est nul (assert seulement dans le SDK) | `kernel_audio_guard.cpp` : suivi des clients, trame ignorée si le client est absent, désinscription sous verrou exclusif |
| 5 | SIGSEGV dans `RtlEnterCriticalSection` appelé par `sub_818EE808` depuis le test (fil titre) ; enregistrement de tas `0x81C061A8` (classe 0) non initialisé | sur la console XAM est chargé aussi dans le processus titre (état « titre », identifiant d'application 238, tas de classe 0) ; ici il n'y a qu'une instance, initialisée dans le processus système (cf. palier A) | `KeGetCurrentProcessType()` renvoie 2 quand l'adresse de retour (`ctx.lr`) est dans l'image de XAM (`SetXamImageRange`) : les fils titre qui entrent dans XAM utilisent l'état système (application 254) |
| 6 | `sub_818A2F90` jamais appelée (non tracée) | `XboxHardwareInfo.Flags` = `0x20` (SDK), XAM teste le bit `0x200` | `FixKernelVariables` : `Flags \|= 0x200` (`0x220`) |
| 7 | l'application 0xFD (`sub_81ABE0F8` → `sub_81BA7FE8` → `sub_81BA7A88`) renvoie E_FAIL et `sub_818951C8` aussi | `ObCreateObject` et `ObInsertObject` sont des stubs : `sub_8187D9A8` (ordonnanceur de tâches audio, objet de type `'Nfto'`) reçoit un objet nul | `kernel_ob.cpp` : corps alloué dans le tas système (en-tête de 16 octets), handles propres (`0xF9000000+`), `ObReferenceObjectByHandle` et `NtClose` les reconnaissent, les autres vont au SDK. Limite : corps jamais libéré, procédures de type non appelées |
| 8 | SIGFPE dans `sub_81BAB930` (`fdivs`, fil audio) | `XAudioGetDucker*` sont des stubs (f1 inchangé, nul) ; les exceptions flottantes hôtes sont démasquées par le code invité | `kernel_audio_guard.cpp` : f1 = 1.0, succès pour les réglages |
| 9 | l'application 0xFA ne revient pas (voir blocages) | `XeKeysGetKey`/`XeKeysExGetKey` stubs : tampon de clé nul | `kernel_keys.cpp` : clé factice (octets pseudo-aléatoires fixes), **seulement pour les appels de XAM** ; insuffisant (voir blocages) |

Les hooks de `common/` sont liés à tous les Dashboards (`xenondash_common`) ; ceux qui dépendent de XAM recompilé (`KeGetCurrentProcessType`,
`XeKeys*`) ne s'activent que pour une adresse de retour dans l'image de XAM ; les autres corrigent des défauts du SDK pour tous
les appelants (minuteries, objets, garde audio, ducker, stockage). Fonctions du SDK appelées en repli par `dlsym(RTLD_NEXT)`
(`kernel_sdk_fn.h`, même interposition ELF que le palier E).

### Résultats des runs (Xvfb `:92`, lavapipe, `DASH_XAM_MODULE=2`, `DASH_XAM_REDIRECT=XamShowMessageBoxUI`, `DASH_TEST_MSGBOX=<s>`)

| Run | État | Résultat de l'appel |
|---|---|---|
| `f6` | correctifs 1 et 2 | `sub_81872230` bloque encore dans `sub_818BEF38` (minuterie) |
| `f7` | + 3 | `sub_818BEF38` revient ; applications enregistrées (entrée 254 : `+8=0xAAAABBBB`, `+12=0x81894D50`, `+16=1`) ; plantage (`RtlEnterCriticalSection`, fil titre) |
| `i1` | + 4, 5 | retour `0x3E5`, aucun plantage |
| `m1`, `m2` | + 7, 8 | `sub_818951C8` dépasse l'application 0xFD ; SIGFPE puis plus de plantage |
| `m4` (150 s) | + 9 | application 0xFA : fil bloqué dans `sub_81A71FD8` à 140 s ; test `0x3E5` ; `0x81BFCAE8..` nul |
| `final1` (45 s) | tout | hub affiché (`runs/final1.png`), retour `0x3E5`, aucun `FATAL`/SIGSEGV/SIGFPE |

### Étape 2 : modules XUI de la flash (constats, recompilation d'essai)

- **Chargement par XAM** : import `xboxkrnl` `XexLoadImage` (`0x81BCEB9C`) avec des noms nus (`hud.xex`, `signin.xex`, ... : table
  de noms `0x817E5464-0x817E55C0` : `deviceselector.xex`, `signin.xex`, `nuihud.xex`, `createprofile.xex`, `gamerprofile.xex`, `vk.xex`)
  ou `\SystemRoot\huduiskin.xex` (`0x817E5FD4`, format `\SystemRoot\%s` en `0x817E25B4`). Le SDK
  (`xboxkrnl_modules.cpp`, `KernelState::LoadUserModule`) résout un nom nu dans le dossier du module exécutable
  (journal : `ResolvePath(hud.xex) failed`). `hud.xex` est demandé **au démarrage de XAM** (journal), pas à l'appel de la boîte.
- **Décodage** (`tools/xex_rekey.py` puis `tools/xexinfo`, fichiers dans `work/`/scratchpad) : `hud`, `signin`, `vk`,
  `createprofile`, `deviceselector`, `gamerprofile`, `ximecore` sont lisibles sans rechiffrement (sortie de `xex_rekey.py`
  identique à l'entrée : clé retail). Ce sont de vrais modules de code :

| Module | Base | Taille image | `.text` | Imports `xam.xex` | Imports `xboxkrnl` |
|---|---|---|---|---|---|
| `hud.xex` | `0x913E0000` | `0x4C000` | `0x1BF6C` | 464 (254 nommés) | 47 |
| `signin.xex` | `0x90100000` | `0x30000` | `0xFC14` | 284 | 22 |
| `createprofile.xex` | `0x90100000` | `0x1E000` | `0x97D8` | 210 | 29 |
| `gamerprofile.xex` | `0x90100000` | `0x4F000` | `0x1F0D8` | 336 | 49 |
| `vk.xex` | `0x917D0000` | `0x17000` | `0x9538` | 152 | 10 |
| `deviceselector.xex` | `0x916D0000` | `0x14000` | `0x618C` | 126 | 9 |
| `ximecore.xex` | `0x91680000` | `0x28000` | `0x1D60C` | 68 | 31 |

  `huduiskin.xex` (70 Ko) est une archive de ressources sans code (chaînes `XamStrings.xus`) : `xexinfo` n'y lit pas de sections.
  Conflit : `signin`, `createprofile` et `gamerprofile` ont la même base `0x90100000` ; la console les relocalise (`.reloc`),
  une recompilation à base fixe ne peut pas les charger ensemble.
- **Recompilation d'essai de `hud.xex`** (dossier `work/hudproj/`, hors dépôt), même procédé que XAM :

```sh
python3 tools/xex_rekey.py <flash>/hud.xex work/hudproj/hud.rekey.xex          # identique à l'entrée
tools/xexinfo/build/xexinfo work/hudproj/hud.rekey.xex work/hud.pe             # image décodée, sections
# manifeste work/hudproj/hud_manifest.toml : [project] name = "hud", [entrypoint] file_path = "assets/hud.xex",
#   out_directory_path = "generated/hud", includes = [hud_overrides.toml, hud_auto_overrides.toml]
python3 tools/codegen_module.py work/hudproj hud_manifest.toml hud work/hud.pe 913E0000 913E6000 1BF6C 913E0400:913E4158 91402000:91402FD4
```

  Résultat : 1133 fonctions connues, 1 cible non résolue (`0x913FC6A4`, écrite dans `hud_auto_overrides.toml`), aucune fonction
  par pointeur, aucune instruction non implémentée, 14 fichiers générés (1,9 Mo), 2 passes de codegen. Les tables de fonctions
  par défaut (`0x9142C000`, `0x91487F58` en vue 0x9) ne recouvrent ni l'image de XAM ni celle du Dashboard ; on peut les placer
  avec `function_table_base` (par exemple `0x84800000`) comme au palier C.
- **Non fait** : déclaration `[[modules]]` de `hud.xex` dans `dash_manifest.toml`, compilation de la DLL, et surtout routage de ses
  imports. Les 464 imports `xam.xex` de `hud.xex` seraient résolus à l'édition de liens contre le HLE du SDK (`__imp__Xui*`),
  pas contre XAM recompilé : il faudrait un hook par ordinal (mécanisme du palier E, généré), et beaucoup d'ordinaux de XAM n'ont
  pas de nom dans la table du SDK (nom nécessaire au symbole `__imp__<Nom>`). Faire charger `hud.xex` par XAM supposerait aussi
  une redirection de `XexLoadImage` pour les noms nus (par exemple vers `game:\flash\<nom>`), car `assets/` ne contient pas les XUI
  à la racine.

### Blocages restants, classés

1. **Application 0xFA (`musicplayer.xex`, `sub_818D19A8`) : démarrage sans fin** (cause non établie). `sub_818951C8` ne revient
   donc pas : les applications 0xF7, 0xF5, 0xF4, 0xF2, 0xF3, 0xF8, puis `sub_818CD070` et la suite de `sub_81872230` (dont
   `sub_818A2F90`, le HUD) ne sont jamais atteintes. Preuves : piles gdb prises à 25 s et 45 s identiques (`sub_818D7110` →
   `sub_818D2DC0` → `sub_81A64D00` → `sub_81A63580` → `sub_81A65638` → `sub_81A72108` → `sub_81A71FD8` → `sub_81A71EE8` →
   `sub_81A71E90`, grands nombres) ; `XeKeysExGetKey(0x100)` (40 octets) et `(0x101)` (20 octets) lus juste avant ; avec le tampon
   nul (stub), même boucle. La clé factice n'est pas une vraie clé RSA : le calcul peut ne pas se terminer sur une entrée
   invalide, ou une instruction du code de grands nombres être mal recompilée (non vérifié). Pistes : fournir une clé 0x100
   valide (module + exposant cohérents), ou instrumenter `sub_81A71EE8` (compteurs de boucle) pour distinguer boucle infinie et
   calcul long.
2. **État du HUD** (`0x81BFCAE8+172/+176`) : posé par `sub_818A2C98`, qui n'est planifiée qu'après le point 1.
3. **XAM et les XUI** : `hud.xex` & co. ne sont pas des `[[modules]]` ; routage des imports non fait (voir étape 2). Rendu : le HUD
   appelle `VdRegisterXamGraphicsNotification` et le pipeline Vd* du noyau (`VdSwap`, `VdGetSystemCommandBuffer`), non examinés ;
   le Dashboard et le HUD partagent le même GPU émulé.
4. **Entrées** (A/B pour la réponse) : non examinées.
5. **Défauts du SDK contournés dans `common/`** (à corriger dans `third_party/rexglue-sdk`, puis retirer les hooks) :
   `UsbdBootEnumerationDoneEvent` de type 1 (`kernel_state.cpp:107`) ; `KeSetTimerEx`/`KeCancelTimer` stubs et
   `GetNativeObject` sans minuteries (`xobject.cpp`) ; `ObCreateObject`/`ObInsertObject` stubs ; `DrvGetContentStorageNotification`
   stub ; `AudioSystem::SubmitFrame` sans test de client nul ; `XAudioGetDucker*`/`XeKeys*` stubs ; `ObReferenceObjectByName`
   (palier D).
6. **Instance unique de XAM** : le correctif 5 (`KeGetCurrentProcessType` == 2 pour XAM) est une approximation : l'état « titre »
   de XAM (identifiant 238, tas de classe 0) n'existe pas ; ce qui en dépend (nom du titre en cours, messages par processus)
   ne peut pas fonctionner.

### Vérifications

| Vérification | Résultat |
|---|---|
| `dashboards/2.0.13604.0-xam` | compile ; hub affiché à 45 s avec `DASH_XAM_REDIRECT=XGetVideoMode,XamShowMessageBoxUI` et le test à 30 s (`final1`) |
| `dashboards/2.0.13604.0` (HLE, même `common/`) | compile (rc 0) ; hub affiché à 40 s (écran `:94`, `runs/hle_final.png`), aucun processus restant |
| `dashboards/2.0.6770.0` | compile (rc 0) ; non lancé |
| SDK (`third_party/rexglue-sdk`) | aucun fichier modifié par ce palier |

### Fichiers

- Nouveaux : `common/src/kernel_timers.cpp`, `common/src/kernel_audio_guard.cpp`, `common/src/kernel_ob.cpp`,
  `common/src/kernel_keys.cpp`, `common/src/kernel_sdk_fn.h`.
- Modifiés : `common/CMakeLists.txt` (4 sources), `common/src/dash_xam_runtime.{h,cpp}` (`FixKernelVariables`, `SetXamImageRange`,
  `InXamImage`, hook `KeGetCurrentProcessType`), `common/src/dash_app_base.cpp` (appels), `common/src/kernel_stubs.cpp`
  (`DrvGetContentStorageNotification`), `common/src/dash_xam_redirect.cpp` (`DASH_TEST_PEEK=<adresse hex>` : 64 mots de mémoire
  invitée avant le test), ce document.
- Hors dépôt : `work/hudproj/` (recompilation d'essai de `hud.xex`) ; traces de diagnostic : générées par un script
  jetable, retirées du code généré avant le dernier build.
