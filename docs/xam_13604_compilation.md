# Compiler XAM 13604 et ses modules : les trois obstacles et leurs contournements

Date : 2026-11-16. Contexte : tentative de faire tourner le parcours de création de profil
d'une vraie console (`DASH_PROFILE_MODE=xam`), en compilant `dashboards/2.0.13604.0-xam`
depuis les fichiers `xeBuild-Data` de la 2.0.13604 fournis par l'utilisateur.

**Résultat : la chaîne compile et XAM se charge, mais l'écran reste noir.** Les trois
contournements ci-dessous sont reproductibles et valent d'être conservés ; le blocage final
est décrit à la fin.

## Les fichiers requis, et leur version

`dashboards/2.0.13604.0-xam/dash_manifest.toml` déclare 8 modules. Les quatre lots proposés
successivement par l'utilisateur ne convenaient pas — la version se lit dans l'en-tête XEX :

| Lot fourni | Version réelle | Verdict |
|---|---|---|
| `SystemUpdate_17559_USB.zip` | 17559 | non : autre version |
| `$flash_dash.xex` + `$flash_xam.xexp` | 7645, et un **delta** `.xexp` | non : NXE, et un xexp s'applique à une base |
| `$flash_dash.xex` + `$flash_xam.xex` | 20141 / 12581 | non |
| **`xeBuild-Data-13604.zip`** | **13604** | **oui** |

Contrôle utile : `tools/xexinfo/build/xexinfo <fichier>` affiche `LIB xboxkrnl.exe v2.0.<version>`.
Pour la 13604 : `xam.xex` doit répondre `v2.0.13604.0`, `BASE 0x817E0000`, `SIZE 0x570000`
après déchiffrement.

## Obstacle 1 — `xam.xex` est chiffré avec la clé XEX1

`xexinfo` renvoie `BASE 0x0 SIZE 0x0` et le codegen échoue :

```
XEX load failed with code 2, trying with devkit encryption key...
XEX load failed with code 2, tried both encryption keys
Failed: Failed to load DLL module: xam
```

C'est exactement le cas décrit dans `docs/technique.md` (« `xam.xex` chiffré avec la clé XEX1 »).

```sh
python3 tools/xex_rekey.py xam.xex xam_rekey.xex    # « clé xex1 -> rechiffrée avec la clé retail »
# puis remplacer assets/flash/xam.xex par le fichier déchiffré
```

`xex_rekey.py` exige `python3-cryptography` (installé dans l'image `xd-builder`).

## Obstacle 2 — `signin`, `createprofile` et `gamerprofile` partagent la même base

Le codegen refuse de générer les 8 modules d'un coup :

```
Failed: Module 'signin' [90100000, 90130000) overlaps 'createprofile' [90100000, 9011E000):
generate overlapping modules one group at a time with --target
```

Ce chevauchement est **voulu** : le manifeste le dit (ligne 37) et c'est cohérent avec la
console — XAM ne charge qu'un de ces trois écrans à la fois, ils occupent donc la même place.

Générer par passes, le manifeste **avant** les options (`--target` consomme les arguments
suivants) :

```sh
rexglue codegen dash_manifest.toml --target xam --target hud --target vk \
    --target deviceselector --target ximecore
rexglue codegen dash_manifest.toml --target signin
rexglue codegen dash_manifest.toml --target createprofile
rexglue codegen dash_manifest.toml --target gamerprofile
```

## Obstacle 3 — chaque passe écrase le registre des modules

C'est le piège le plus coûteux. `rexglue codegen --target` **régénère entièrement** deux
fichiers, qui ne gardent donc que le module de la dernière passe :

- `generated/dash/dll_targets.cmake` (les cibles CMake des `.so`)
- `generated/dash/module_registry.cpp` (ce que `FindRecompiledModule` peut trouver)

Symptômes, dans l'ordre où ils apparaissent :

```
[dash] DASH_XAM_MODULE défini mais le projet ne déclare pas de module xam.xex
```
puis, une fois le registre complété :
```
File not found: game:\xam.xex          <- le fichier n'était pas au bon endroit
```

Contournement : après les quatre passes, réassembler les deux fichiers à la main.

- `dll_targets.cmake` : concaténer un bloc `# DLL module: <nom>` … `endif()` par module
  (`xd-fix-dlltargets.sh` le fait automatiquement en sauvegardant chaque bloc au fil des passes).
- `module_registry.cpp` : un `RegisterRecompiledModule(pe, guest_path, lib)` par module. Le
  `guest_path` doit être **normalisé** (`NormalizeGuestPath` : minuscules, sans préfixe device),
  donc `"xam.xex"` et non `"flash/xam.xex"` — sinon `FindRecompiledModule("xam.xex")` ne trouve rien.

## Le chargement exige le fichier, pas seulement le module

`LoadUserModule("game:\\xam.xex")` ouvre un **fichier** par le VFS avant de câbler le code
recompilé. `game:` correspond à la racine des assets du projet : le fichier doit donc être en
`assets/xam.xex` (la doc le dit : « `game:\xam.xex` est lu depuis `assets/xam.xex` »), et non
seulement en `assets/flash/`.

Avec ces corrections :

```
[dash] module xam.xex recompilé : chargé
[xamredir] DASH_PROFILE_MODE=xam : redirections XGetVideoMode,XamUserGetXUID,…,
           XamShowKeyboardUI,XamShowSigninUI,XamShowSigninUIp,XamShowCreateProfileUI,…
[xamredir] XamShowCreateProfileUI (ordinal …) -> XAM recompilé 0x…
```

Les écrans du parcours de création sont donc bien redirigés vers le XAM recompilé.

## Quatre défauts corrigés, et l'état réel

### 1. Gestionnaire de faute du SDK (branche `unified`, commit `7348c20`)

`AccessViolationCallback` refusait tout heap autre que `kGuestPhysical` ; `SystemHeapAlloc` alloue
dans `v00000000` (`kGuestVirtual`). Le premier écrivain d'un module recompilé dans une de ces
allocations provoquait un « Unhandled guest access violation » définitif. Seul le bloc
`TriggerCallbacks` est spécifique aux heaps physiques ; le chemin de récupération
(`QueryProtect` / `QueryRegionInfo` / `Protect`) est générique.

**Mesure : violations 1 → 0.**

### 2. Registre des modules : préfixe `flash/`

Les modules XUI sont chargés par XAM sous `game:\flash\<nom>.xex`, que `NormalizeGuestPath` réduit
à `flash/<nom>.xex`. Le registre portait `"<nom>.xex"` : `FindRecompiledModule` échouait, donc
aucune table de dispatch, donc `Execute(…): function not in function table`.

**Mesure : tables de dispatch 2 → 4 ; `function not in function table` : 0.**

### 3. `KeSetTimerEx` transmet le DPC (commit `0111890`)

Le hook passait `(0, 0)` comme routine et contexte à `XTimer::SetTimer` : la routine différée
n'était **jamais** appelée. XAM en pose une pendant son initialisation
(`KeSetTimerEx : DPC 0x81CA9234 ignoré`).

**Mesure : le message a disparu (0 occurrence).**

### 4. Chargement de XAM asynchrone (commit `ea19503`)

`RunInSystemProcess` créait un `XHostThread` puis appelait `t->Wait(…)` : synchrone malgré le
thread. `DASH_XAM_SYNC=1` rétablit l'ancien chemin.

**Mesure : XAM se charge sur un thread de travail (`t41`), plus sur le principal (`t12`).**

## Où ça bloque : après le chargement, dans les stubs noyau

L'écran reste noir. Ce qui est établi, et qui **corrige deux de mes conclusions précédentes** :

| Vérification | Résultat |
|---|---|
| rendu natif initialisé | **oui** — `[ngpu] noyau ngpu_core sur 'llvmpipe …' (profil 13604)` |
| où le lire | **sur stderr**, jamais dans `dash.log` |
| thread principal | **en attente X11 normale** (`SDL_WaitEventTimeout`), pas bloqué |
| durée du journal | **~3 s**, alors que le processus vit 40 s |
| fautes invitées / FATAL | 0 / 0 |

Aucun thread invité ne demande plus de présentation : le Dashboard cesse de progresser.

Après le chargement, une trentaine de threads démarrent et butent **chacun sur un stub
différent** :

```
EtxProducerRegister (15)  XeKeysGetStatus (5)   HalSendSMCMessage (5)
NicGetLinkState (4)       XeKeysExSetKey (3)    MtpdGetCurrentDevices (3)
EtxConsumer* (6)          DumpRegisterDedicatedDataBlock (3)  UsbdGetNatalHardwareVersion (2)
```

Aucun ne bloque **à lui seul** (vérifiés un par un) : c'est l'ensemble qui n'aboutit jamais à une
frame. C'est le symptôme d'un XAM recompilé qui attend des fonctions noyau que le SDK n'implémente
pas — le palier « XAM recompilé » de `docs/xam_recompile.md` est en cours, pas terminé.

### Contrainte à ne pas oublier

`XeKeysGetStatus` et `XeKeysExSetKey` touchent aux clés de console, et `REPRISE.md` interdit d'en
fabriquer. Toute implémentation doit rester dans la ligne du palier N4 : clé locale tirée au hasard,
`XeKeysObfuscate`/`UnObfuscate` locaux (déjà dans `common/src/kernel_obfuscate.cpp`), jamais de clé
de console réelle.

## Correction : ce que montre vraiment l'écran

Une capture de 690 Ko (`xam-demarrage.png`, prise à 00:56) montrait le Dashboard complet en vert,
onglets Xbox LIVE / My Xbox / Welcome, tuiles Kinect. Elle laissait croire que la variante `-xam`
s'affichait par moments.

**C'est faux, et la mesure le prouve.** Le même binaire, lancé trois fois :

| Mode | Variable d'environnement | Capture | Journal |
|---|---|---|---|
| A | *(aucune)* | **690 625 octets** — Dashboard affiché | 21 s, `xam.xex` **non chargé** |
| B | `DASH_PROFILE_MODE=xam DASH_XAM_MODULE=2` | 3 073 octets — noir | 2 s, `xam.xex` chargé |
| C | idem B | 3 073 octets — noir | 2 s (déterministe) |

**`DASH_PROFILE_MODE=xam` est ce qui active les redirections** (`ProfileModeXam()` dans
`dash_xam_redirect.cpp` : il faut la variable *et* `DASH_XAM_MODULE`). Sans la variable, le
Dashboard répond par le **HLE** : XAM n'est même pas chargé, et l'interface s'affiche normalement.

Donc la capture de 00:56 était le Dashboard en mode HLE, pas le XAM recompilé. Le noir est bien
systématique dès que XAM pilote.

### Conséquence pour le diagnostic

Il faut comparer ce qui est comparable : « 21 s contre 2 s » n'oppose pas deux réglages du même
chemin, mais **deux chemins différents** (HLE contre XAM). Le journal de 2 s est celui de XAM qui se
charge puis se tait.

## L'écran noir : zéro dessin, et la cause est en amont du rendu

Mesure comparative des deux modes, avec `DASH_NGPU_STATS=1` et `DASH_NGPU_EVLOG=1` (16 s de
session, même machine) :

| | mode HLE (`2.0.13604.0`) | mode XAM (`DASH_PROFILE_MODE=xam`) |
|---|---|---|
| image présentée | **1** (`resolves (color) : 1`) | **aucune** |
| événement `[ev]` du rendu natif | oui | **aucun** |
| `D3DDevice capture` | 1 | **0** |
| appels D3D du code invité | oui | **0** |

**Le rendu natif n'est pas en cause : il n'est jamais sollicité.** Sans `D3DDevice` capturé, aucun
crochet D3D ne s'installe, donc le Dashboard n'émet pas un seul `BeginTiling` ni `Swap`. Chercher un
bug de composition ou de Vulkan serait une fausse piste.

### Ce qui n'est jamais appelé

| appel | mode HLE | mode XAM |
|---|---|---|
| `XamContentAggregateCreateEnumerator` | oui | **jamais** |
| `XamProfileCreateEnumerator` / `XamProfileOpen` | oui | **jamais** |
| `XamReadTile` | oui | **jamais** |

En mode XAM, le Dashboard n'entre **jamais** dans le chargement de son interface : il s'arrête juste
après `XamLoaderGetMediaInfo` et une notification de média.

### Percée : le HUD atteint enfin son D3D

La chaîne d'événements complète, tracée depuis `sub_81872460` (atteint) :

```
sub_81872460 -> sub_8185C830 -> sub_818A5A98 -> publie 0x818A5A08
```

Instrumentation de `sub_8185C830` et `sub_818A5A98` : **les deux sont atteints**. La chaîne s'exécute.

Et pour la première fois depuis le début du chantier, le HUD **appelle son D3D** :

```
[hud] D3D 2099 #1 addr=818A1368 (kFrame)
[hud] D3D 2099 #2 addr=819C90D8 (kDrawVerticesUP)
[hud] D3D 2099 #3 addr=819C8E38 (kBeginVertices)
[hud] noyau ngpu_core sur 'llvmpipe ...' (HUD de XAM)   <- noyau propre du HUD initialisé
```

C'était mesuré à **zéro** pendant six sessions. Le HUD est donc vivant, il soumet des commandes et
initialise son propre noyau de rendu.

### Ce qui manque encore

| | mesure |
|---|---|
| appels D3D du HUD | **4** (kFrame, kDrawVerticesUP, kBeginVertices) |
| `HudComposite` compose | **non** (`HUD affiche` jamais émis) |
| image présentée par le Dashboard | **0** |
| `D3DDevice` du Dashboard capturé | **0** |

Deux problèmes distincts subsistent : le HUD ne produit pas encore d'image complète (`kSubmit` jamais
atteint, donc `VdSetSystemCommandBuffer` muet), et le Dashboard ne présente toujours rien — sans
présentation, `HudComposite` n'est même pas appelé.

**C'est un progrès réel** : la couche HUD n'est plus muette, elle est amorcée.

### Correction de modèle : `sub_81891BA8` n'est pas la machinerie du HUD

J'ai vérifié trois choses qui invalident mon modèle précédent :

1. **`sub_81891BA8` est appelée depuis plus de 60 sites** dans tout `xam.xex` — ce n'est pas une
   fonction du HUD, c'est un **utilitaire générique de publication d'événements**.
2. **Aucun site `ExCreateThread` ne référence la zone `0x818A`** (vérifié sur les 8 sites).
3. **L'inventaire des threads créés ne contient que des entrées en `0x922...`** (code du Dashboard),
   jamais `0x818A5A08`.

Et `0x818A5A08` n'est pas un point d'entrée de thread : il est **rangé dans une structure d'état**
(`stw r30,48(r31)` dans `sub_81891478`), qui est ensuite traitée par une machine à états
(`r11 & 0x1F` = 1, 2, 4...).

**Conséquence** : ma piste « la chaîne casse à la création du thread HUD » était mal posée. Le HUD
n'a pas de thread à créer par ce chemin — il s'enregistre comme gestionnaire d'événements, et c'est
un thread **déjà existant** qui devrait le servir.

Il faut donc chercher ailleurs : quel thread existant consomme cette structure d'état, et pourquoi il
ne le fait pas.

### Les cinq candidats mènent à des threads qui ne sont pas le HUD

Instrumentation des cinq fonctions appelées par `sub_81891478` : `sub_8188F200`, `sub_818908E0`,
`sub_81890650`, `sub_8188FE08`, `sub_81890440`. Aucune ne contient `ExCreateThread`.

Quatre sont atteintes (`8188F200`, `8188FE08`, `81890440`, `81890650`) ; ce sont des fonctions de
synchronisation (critical region, spinlock), pas de création.

Le vrai site de création est `sub_8188F6E8`, instrumenté aussi :

```
[c6] sub_8188F6E8 (cree un thread) r3=0x81890F18 r4=0x402E06A0
[c6] sub_8188F6E8 (cree un thread) r3=0x81890F18 r4=0x402E06E0
...
```

**Il crée bien des threads — mais avec l'entrée `0x81890F18`, pas `0x818A5A08`.** Ce sont les threads
de `xam`, pas ceux du HUD. Le thread du HUD, lui, est passé à `sub_81891BA8` (qui protège une
structure) et n'aboutit jamais à une création.

**Conclusion** : le HUD et `xam` utilisent deux mécanismes distincts. `xam` crée ses threads par
`sub_8188F6E8` → `ExCreateThread` ; le HUD demande le sien par `sub_81891BA8`, qui ne crée rien.
Le chemin manquant est donc **entre `sub_81891BA8` et une création de thread** — pas dans les cinq
fonctions testées ici.

### Le contrôle de privilège n'est pas le verrou

En remontant `sub_81891BA8` (qui protège une structure avec `KeEnterCriticalRegion` et un spinlock,
mais **ne crée aucun thread**) jusqu'à `sub_81891478`, on trouve un appel à
`XexCheckExecutablePrivilege(priv=1)` avant le lancement.

L'hypothèse était solide : le SDK accorde le privilège seulement si `HEADER_SYSTEM_FLAGS` du XEX
contient le bit demandé, et ces en-têtes sont **chiffrés** dans les fichiers fournis. Un échec
expliquerait que le thread ne soit jamais créé.

**Test A/B** : hook installé pour forcer `XexCheckExecutablePrivilege` à renvoyer 1.

| mesure | résultat |
|---|---|
| privilèges demandés dans la session | `priv=10`, `priv=1`, `priv=14`, `priv=2` |
| forçage actif (`priv=1` → 1) | oui |
| thread HUD (`sub_818A5A08`) exécuté | **non** |
| appel D3D du HUD | **aucun** |

**Hypothèse réfutée** : le privilège n'est pas ce qui bloque. Le hook a été retiré (il aurait faussé
le comportement, et le SDK reprend la main).

Le chemin de création reste donc à trouver ailleurs : `sub_81891478` appelle ensuite `sub_8188F200`,
`sub_818908E0`, `sub_81890650`, `sub_8188FE08`, `sub_81890440` — cinq candidats, dont l'un devrait
mener à `ExCreateThread`.

### Le thread du HUD est demandé, mais n'est jamais exécuté

Instrumentation des trois points chauds de `sub_81872460` :

| point | mesure |
|---|---|
| `sub_81891BA8` (création de thread) | appelée **plusieurs fois** |
| entrées passées en `r3` | `0x81872180`, `0x8185C5E0`, **`0x818A5A08`** |
| `sub_8188F7C0` (suite) | appelée |
| **`sub_818A5A08`** (routine du thread HUD) | **jamais exécutée** |

`0x818A5A08` est bien dans la zone HUD (`0x818A...`) et existe comme fonction. Elle est donc
**demandée comme point d'entrée de thread**, mais son code ne s'exécute jamais.

Note de méthode : mon calcul `lis`/`addi` d'origine donnait `0x81872230` alors que `r3` valait
`0x81872180` — 176 octets d'écart. L'erreur venait de l'ajustement de signe sur `lis` ; seules les
valeurs **mesurées à l'exécution** sont fiables, pas les adresses recalculées à la main.

**Conclusion** : la chaîne ne casse plus au niveau du code XAM — qui demande bien le thread — mais au
niveau de la **création/exécution du thread invité**. C'est là qu'il faut regarder ensuite
(`sub_81891BA8` et son chemin vers `ExCreateThread`).

### `sub_818A2500` : la chaîne progresse d'un maillon, puis s'arrête

Instrumentations successives du code généré, exécutées dans `libdash_xam.so` :

| point | résultat |
|---|---|
| `xstart` de `xam.xex` | **atteint**, `r3=0x30020000` (hmodule), `r4=1` (DLL_PROCESS_ATTACH) |
| `sub_81872460` | **atteint** (condition `r4 == 1` remplie) |
| `sub_818A2500` (boucle d'image du HUD) | **jamais atteint** |

Le maillon `sub_81872460` est donc franchi — c'est nouveau. Mais la chaîne casse ensuite.

Dans `sub_81872460`, `0x81872230` est passé en `r3` à `sub_81891BA8`, et le handle obtenu part dans
`sub_8188F7C0`. Autrement dit, `sub_81872230` est fourni comme **point d'entrée d'un thread** — c'est
un thread qui doit l'exécuter, pas un appel direct.

Et `sub_81872230` n'est **jamais appelé directement** dans tout le code recompilé : ni `sub_81872230(ctx)`,
ni `lis`/`addi` vers cette adresse ailleurs que ce site. Il n'existe que comme valeur.

**Conclusion** : le lancement du sous-système HUD dépend de la création effective d'un thread à partir
de `sub_81872460`. Le maillon est atteint, la création n'aboutit pas — ou le thread créé ne s'exécute
pas. C'est le point précis à instrumenter ensuite.

### La cause de l'erreur fatale : mon propre code

En traçant le statut comparé dans `sub_8187A038` (instrumentation du code généré, exécutée dans
`libdash_xam.so`) :

```
[bisect] sub_81878400 entree r3=1076758784 r4=1884356320
[bisect] statut -> 0xC0000135
```

`0xC0000135` = **`STATUS_DLL_NOT_FOUND`** — ni `0xC0050005` ni `0xC005000F` (les deux statuts
tolérés), donc branche fatale.

Or **c'est exactement le code que mon hook `XexLoadExecutable` renvoyait**, avec le commentaire
« honnête, on n'a rien chargé ». Un échec honnête à cet endroit coûte l'affichage entier : XAM le
propage, le compare à ses statuts tolérés, échoue, et déclenche l'erreur fatale d'affichage.

**Corrigé** : `XexLoadExecutable` renvoie 0 (succès) au lieu de `STATUS_DLL_NOT_FOUND`.

**Mesure** : les erreurs fatales passent de **1 à 0**. Plus aucun `VdDisplayFatalError` dans toute la
session. L'écran reste noir et le HUD n'émet toujours pas de D3D — mais la dernière erreur que XAM
signalait a disparu.

C'est le second compteur qui recule après une correction (2 → 1 → 0 sur les erreurs fatales).

### Bissection instrumentée : les deux sites identifiés par leur adresse de retour

Instrumentation de `VdDisplayFatalError` (journal du code et de `lr`, l'adresse de retour). Résultat
sans ambiguïté, **les deux sites tirent** :

| `lr` | site | code |
|---|---|---|
| `0x81872278` | `sub_81872230` | `0x00012348` |
| `0x8187A2F8` | `sub_8187A038` | `0x00012247` |

**Et la polarité du premier site était l'inverse de ce que j'avais lu.** Le code recompilé dit :

```
bl sub_81892A60        ; renvoie le bit 0 de l'octet 4 de HalGetPowerUpCause
clrlwi. r11,r3,24
beq loc_81872298       ; si 0 -> SAUTE l'erreur
<erreur fatale>
```

Autrement dit : bit à **0** = pas d'erreur ; bit à **1** = erreur fatale. Mon implémentation
précédente mettait 1 — elle **provoquait** l'erreur qu'elle prétendait éviter. Corrigé (bit à 0).

**Mesure après correction** : les erreurs fatales passent de **2 à 1**. Celle du premier site a bien
disparu. C'est la première fois qu'une correction fait reculer un compteur mesuré.

Reste celle de `sub_8187A038` (`0x12247`), qui est une machine à états comparant un statut reçu à des
codes `0xC005xxxx` (domaine vidéo).

### Bissection : `HalGetPowerUpCause`, une piste sérieuse qui n'aboutit pas

Deux sites appellent `VdDisplayFatalError` (« erreur fatale d'affichage ») dans `xam.xex` :

| site | code d'erreur | condition |
|---|---|---|
| `sub_81872230` | `0x00012348` | `sub_81892A60()` renvoie 0 |
| `sub_8187A038` | `0x00012237` | machine à états (`r30` ≠ `r29` et ≠ `r24`) |

`sub_81892A60` lit l'octet à l'offset 4 d'une variable globale (`0x81BFB854`) et renvoie son bit 0.
Or cette variable est celle que remplit **`HalGetPowerUpCause`**, un **stub du SDK** : la mémoire
restait à zéro, donc le bit valait 0, donc XAM concluait à une panne d'affichage. La chaîne semblait
donc complète et la correction évidente.

**Implémenté** : `HalGetPowerUpCause` écrit une mise sous tension normale (octet 0 = 1, octet 4 = 1).
Mesure : la structure est bien écrite — `01 00 00 00 01 00 00 00`, bit 0 de l'octet 4 armé.

**Résultat : `VdDisplayFatalError` est toujours appelé et l'écran reste noir.** Ce n'était donc pas
cette condition, ou pas seulement elle. L'autre site (`sub_8187A038`, machine à états) reste un
candidat, ainsi que des appels venant d'ailleurs.

Cinquième instrumentation utile au passage : `HalGetPowerUpCause` répond désormais correctement, ce
qui reste juste indépendamment de l'écran.

### Expérience : charger un autre module que xam.xex

Objectif : distinguer « c'est le chargement (la machinerie) qui casse l'affichage » de « c'est XAM
(son contenu) ». Ajout de `DASH_XAM_ALT=<nom>` qui charge un autre module à la place.

| module demandé | trouvé | capture |
|---|---|---|
| *(aucun)* | — | **690 625 o** (image) |
| `hud.xex` | **non** (`File not found: game:\\hud.xex`) | **690 625 o** (image) |
| `vk.xex` | **non** (`File not found: game:\\vk.xex`) | **690 625 o** (image) |

Ces deux-là ne sont pas trouvés (ils sont dans `assets/flash/`, pas à la racine de `game:`) : rien
n'est chargé, donc l'image reste — ce qui ne prouvait rien de plus.

**Le test qui tranche**, avec un module réellement résolvable :

| module demandé | trouvé | capture |
|---|---|---|
| `flash\hud.xex` | **oui** (`registered 1133 functions`) | **690 625 o** (image) |

**La machinerie de chargement n'est donc pas en cause.** Charger un vrai module de 1133 fonctions,
avec tout ce que cela implique (allocation, registre, imports), **laisse l'affichage intact**. C'est
spécifiquement `xam.xex` qui le coupe.

Le problème est donc dans ce que `xam.xex` *fait* — pas dans le fait de le charger. Cela recadre la
recherche : ce sont ses appels noyau et son initialisation qu'il faut regarder, ce que la suite de ce
document décrit.

### La boîte de message bloquante : hypothèse réfutée

L'hypothèse était plausible : `XamShowMessageBoxUI` est **bloquante**, elle est redirigée vers XAM
depuis le palier N5, et XAM n'affiche rien — le Dashboard attendrait donc indéfiniment.

Test propre cette fois (les tentatives précédentes comparaient des journaux vides), avec
`DASH_XAM_NOREDIRECT=XamShowMessageBoxUI,XamShowMessageBoxUIEx` :

| | retrait effectif | capture | énumérations de contenu | capture D3D |
|---|---|---|---|---|
| sans XAM (référence) | — | **690 625 o** (image) | oui | oui |
| XAM, MessageBox **non** redirigée | **oui** (`RETIREE` ×2) | **3 073 o** (noir) | **aucune** | **non** |

Le retrait est bien appliqué, et l'écran reste noir. **L'hypothèse est réfutée** : la boîte de
message n'est pas ce qui bloque le Dashboard. Le HLE répond, et le code ne progresse pas davantage.

C'est la cinquième piste fermée par la mesure, après la composition Vulkan, l'interblocage d'objets
noyau, le HUD « cassé », le mode profil et le périphérique D3D oscillant.

### `XexStartExecutable` implémenté : exécuté, mais insuffisant

`XexStartExecutable` était un **stub du SDK** alors que XAM l'appelle sur le chemin du HUD :
`XexLoadExecutable` → `XexStartExecutable(0x81879B48)` → `ExCreateThread` (thread système).

Implémenté dans `xam_kernel_stubs.cpp` : la fonction passée en `r3` est exécutée sur un fil système,
via `RunInSystemProcessAsync` du projet. Mesure : l'appel a bien lieu
(`[xex] XexStartExecutable(0x81879B48) : demarrage sur un fil systeme`).

**Mais `sub_81872230` (le sous-système HUD) n'est toujours pas atteint**, l'écran reste noir et le HUD
n'émet aucun appel D3D. L'exécutable démarré n'est donc pas celui qui porte la chaîne du HUD — ou il
lui manque une entrée antérieure.

### Ce que la comparaison des deux journaux montre

En mode sans XAM, la séquence de démarrage va jusqu'aux énumérations de contenu
(`XamContentAggregateCreateEnumerator`, `XamProfileCreateEnumerator`). En mode XAM, elle s'arrête
après une série de stubs de sécurité et de démarrage :

```
XexLoadExecutable STUB  ->  XeKeysSetProtectedFlag STUB  ->  XexStartExecutable  ->
ExCreateThread (systeme)
```

Ces trois appels forment la séquence de démarrage d'un exécutable par XAM. Les deux premiers sont
des stubs : l'exécutable n'est donc ni chargé ni protégé comme sur console.

À noter, sans en tirer de conclusion : un journal antérieur montrait aussi `VdDisplayFatalError`
juste après la création du thread système, sur un run où la séquence allait plus loin. À vérifier si
ce n'est pas simplement l'ordre des fils qui diffère d'un run à l'autre.

### Côté Dashboard aussi, la couche D3D n'est jamais atteinte

Instrumentation `DASH_NGPU_DEVCAND=1` (trace de chaque candidat de périphérique D3D) :

| | candidats device | capture | événements `[ngpu]` |
|---|---|---|---|
| sans XAM | `40002200` (plausible, puis retenu) | **1** | 4 |
| avec XAM | **aucun** | **0** | 1 (le noyau seul) |

Le Dashboard en mode XAM n'appelle donc **même pas** les fonctions D3D crochetées : ce n'est pas un
problème de capture du périphérique (le candidat oscillerait ou serait rejeté), c'est que le code
n'arrive jamais à la couche de dessin.

Cela recoupe exactement la mesure faite côté HUD : ni l'un ni l'autre ne dessine, et pour la même
raison — la chaîne qui mène au dessin n'est pas exécutée.

### La chaîne de lancement du HUD, remontée jusqu'à son portier

Les adresses PowerPC ne se cherchent pas comme du texte : `0x818A2500` est chargé par `lis` + `addi`,
donc un `grep` sur le littéral ne trouve rien. En cherchant le motif (`lis -32374` + `addi 9472`),
la chaîne apparaît :

```
sub_81872460  prépare le lancement (stocke 0x81872230 dans une structure)
  └─ sub_81872230  point d'entrée du sous-système HUD
       └─ sub_818A2F90   (sous-système HUD)
            └─ sub_818A2500  boucle d'image du HUD
                 └─ sub_818A1B58 -> kFrame -> kSubmit -> VdSetSystemCommandBuffer
```

Le portier de `sub_81872230` est identifié précisément :

```
sub_818B5028(...)          -> ObCreateSymbolicLink : renvoie un statut
if (r3 >= 0) sub_818A2F90()   <- le HUD ne démarre QUE si le lien symbolique est créé
```

`ObCreateSymbolicLink` est **implémenté** (SDK et `common/src/kernel_ob.cpp`), donc la condition peut
passer. Mais `sub_81872230` n'est **jamais appelé par du code** et n'apparaît dans **aucun journal** :
il est seulement stocké comme valeur par `sub_81872460`.

Enfin, en aval, `HalNotifyAddRemoveBackgroundTask` (dans `sub_81891368`, sur le chemin de création du
thread) est un **stub** du SDK, et n'est **jamais appelé** dans nos sessions.

### Ce que cela signifie pour la suite

La cause n'est plus « on ne sait pas où ça bloque » : c'est une chaîne identifiée, avec un portier
nommé (`ObCreateSymbolicLink`) et un déclencheur manquant (`sub_81872460` n'est pas exécuté). La
question est devenue : **qu'est-ce qui, sur console, exécute `sub_81872460` ?**

### Le HUD n'a aucun thread de rendu

La chaîne du dessin du HUD, suivie dans le code recompilé :

```
sub_818A2500   (boucle d'image)
  └─ sub_818A1B58
       └─ sub_819CB710  -> VdQuerySystemCommandBuffer   (notre hook renvoie 1 pour XAM : passe)
       └─ sub_818A1368  (kFrame)
            └─ sub_819CC490  (kSubmit) -> VdSetSystemCommandBuffer
```

Le portier (`VdQuerySystemCommandBuffer`) est franchi : notre hook renvoie **1** quand l'appelant est
dans l'image XAM, et `InXamImage()` est vrai pour ces adresses (vérifié). La chaîne est donc
fonctionnelle **si elle est exécutée**.

Or `sub_818A2500` n'est **jamais appelée par du code**, et n'est **jamais référencée comme pointeur**
(sauf sa définition et son enregistrement). C'est une routine destinée à un **thread créé par
adresse**.

Mesure avec `REX_LOG_THREADS=1` (instrumentation ajoutée dans le SDK, `ExCreateThread`) : les 11
threads invités créés ont pour entrées `0x92170960`, `0x92172AB8`, `0x9216DF08`, `0x921AE620`,
`0x92249730`, `0x92249488` (code du Dashboard) et `0x81890F18` (XAM). **Aucun n'a `0x818A2500`.**

**Conclusion : le HUD ne lance jamais son thread de rendu.** C'est pourquoi aucun appel D3D n'est
émis, pourquoi `kSubmit` n'est jamais atteint, et pourquoi `VdSetSystemCommandBuffer` reste muet.
Les trois symptômes ont enfin une cause unique.

La question suivante est donc précise : quel événement ou quel appel, sur console, provoque la
création de ce thread ? C'est là que se situe le défaut.

### C'est le chargement, pas l'exécution

`DASH_XAM_MODULE=1` charge `xam.xex` **sans** exécuter son point d'entrée ; `=2` l'exécute en plus.
Comparaison des captures :

| | capture | où va le Dashboard |
|---|---|---|
| sans XAM | **image** (690 625 o) | démarrage normal |
| `DASH_XAM_MODULE=1` (chargé, entrée non exécutée) | **noir** | énumère les titres, lit le média |
| `DASH_XAM_MODULE=2` (chargé + entrée) | **noir** | idem |

**Il suffit de charger `xam.xex`** pour que le Dashboard cesse d'afficher : son point d'entrée n'y est
pour rien. Et il ne bloque pas — il va même assez loin (énumération des titres, `XamEnumerate`) —
mais n'émet **aucun appel D3D**.

Hypothèses testées et écartées sur ce chemin :
- `MountXamDevices` qui remplace `\\Device\\Harddisk0\\Partition1` : **aucun fichier manquant** dans le
  journal, donc le Dashboard ne perd pas ses assets ;
- `FixKernelVariables` : les deux drapeaux sont posés correctement (vérifié) ;
- boucle réseau du HUD : corrigée, sans effet sur l'affichage.

Suspect restant, non encore levé : `VdSetSystemCommandBuffer`, qui d'après le commentaire du code
est appelé par XAM « à chaque image du HUD ». Mesure : il n'est **jamais appelé**. Le HUD ne va donc
pas jusqu'à soumettre ses commandes GPU — ce qui est cohérent avec l'absence de tout appel D3D.

### L'initialisation du HUD est confirmée par la trace des DllMain

Instrumentation `REX_LOG_DLLMAIN=1` dans le SDK (`kernel_state.cpp`, au point d'appel de
`DllMain(DLL_PROCESS_ATTACH)`), qui répond à une question restée ouverte : l'entrée d'un module
est-elle seulement exécutée ?

```
[dllmain] 'xam'      entry=81872658 -> DLL_PROCESS_ATTACH
[dllmain] 'ximecore' entry=91689F78 -> DLL_PROCESS_ATTACH
[dllmain] 'hud'      entry=913FBC70 -> DLL_PROCESS_ATTACH
```

Les trois sont appelés, et `xstart` de `hud.xex` s'exécute (`[hud] xstart APPELE`). Le HUD
**s'initialise donc réellement** — ce n'était pas le doute à avoir.

### Une boucle réseau dans l'initialisation du HUD, corrigée

Juste après `xstart`, le fil du HUD part dans une boucle d'appels réseau : `MtpdGetCurrentDevices`
(15 fois) et `NicGetLinkState` (10 fois), tous deux en `REX_EXPORT_STUB`. Un stub laisse `r3` à une
valeur arbitraire : le HUD lisait donc un état de lien aléatoire.

Les deux sont maintenant implémentés (`xam_kernel_stubs.cpp`) :
- `NicGetLinkState` : écrit 0 dans la sortie (aucun lien), succès ;
- `MtpdGetCurrentDevices` : succès, liste vide (aucun périphérique MTP).

Mesure après correction : les deux stubs disparaissent du journal, la boucle réseau s'arrête. **Mais
l'écran reste noir** : ce n'était pas le blocage non plus, seulement du bruit en moins.

### Où en est le compte des stubs

Après ces corrections, il reste 57 lignes de stub dans une session de 20 s, dont les plus fréquentes :
`XamPackageManagerGetExperienceMode` (4), `XeKeysExSetKey` (3), `EtxConsumerProcessLogs` (3),
`DumpRegisterDedicatedDataBlock` (3), `UsbdGetNatalHardwareVersion` (2). Aucune n'a le volume d'une
boucle d'attente : le blocage n'est plus dans les stubs.

### La cause est le chargement de XAM lui-même, pas le mode profil

Test en trois temps, capture d'écran à chaque fois (690 Ko = image, 3 Ko = noir) :

| combinaison | résultat |
|---|---|
| mode HLE seul | **image** |
| `DASH_XAM_MODULE=2` seul, sans mode profil | **noir** |
| `DASH_XAM_MODULE=2` + interfaces redirigées vers XAM | **noir** |

**Il suffit de charger XAM** pour que le Dashboard cesse d'afficher : ni `DASH_PROFILE_MODE`, ni les
redirections n'y sont pour quoi que ce soit. C'est plus précis que le diagnostic précédent.

Le chargement est bien asynchrone (`XAM load` sur son propre fil, `RunInSystemProcessAsync`), donc
ce n'est pas un blocage du fil principal pendant le chargement : la capture est noire dès 4 s et le
reste à 8 s et 16 s.

### Ce qui, dans le chargement, est déjà correct

Vérifié pas à pas, pour ne pas refaire le travail :

- `XboxHardwareInfo.Flags` passe bien à **`0x220`** (0x20 + 0x200), condition que le code de XAM
  exige pour lancer son sous-système HUD ;
- `hud.xex` **est chargé** : `XexLoadImage("hud.xex") -> game:\\flash`, puis
  `Module 'flash/hud.xex' registered 1133 functions` ;
- `XamRegisterSysApp` est appelé et servi par XAM recompilé ;
- `VdRegisterXamGraphicsNotification` est **déjà implémenté** (`kernel_video.cpp`, pas un stub) et il
  fonctionne : XAM s'enregistre (`0x818A04F8`) puis reçoit le message 2 et le message 1
  (`VdCallGraphicsNotificationRoutines`).

Autrement dit, XAM démarre, charge le HUD et s'inscrit aux notifications graphiques. Ce qui manque
est **après** : la couche de dessin n'est jamais atteinte, ni par le Dashboard ni par le HUD.

### Le HUD de XAM est chargé, mais son D3D n'est jamais appelé

`hud_front.cpp` (le HUD de XAM : fenêtres Xbox, clavier virtuel, création de profil, et de quoi
faire un vrai Guide) est compilé et lié (`NGPU_HUD=ON`, 1348 fonctions de `xam.xex` crochetées).
Il était donc naturel de penser qu'il suffisait de le solliciter.

**Mesure avec `DASH_HUD_TRACE=1`** (instrumentation ajoutée pour l'occasion) : le crochet commun
`hudd3d_call` est bien atteint **plus de 25 000 fois** en 22 s. Mais en distinguant les adresses :

| adresse | nature | appels |
|---|---|---|
| `0x819BD640`, `0x819B7BC0`, ... | fonctions internes de `xam.xex` | **oui**, des milliers |
| `0x819C8E38` (`kBeginVertices`) | D3D 2099 du HUD | **jamais** |
| `0x819C90D8` (`kDrawVerticesUP`) | D3D 2099 du HUD | **jamais** |
| `0x819CA3E0` (`kResolve`) | D3D 2099 du HUD | **jamais** |

Les adresses D3D 2099 sont **valides** (présentes dans `xam_d3d_entries.inc` et dans le code
recompilé) : ce n'est pas un problème de génération.

**Conclusion : XAM recompilé n'appelle jamais son propre D3D.** Il exécute du code (des milliers
d'appels internes) mais n'atteint pas la couche de dessin — donc le HUD n'a jamais rien à composer,
et `HudComposite` ne superpose rien. Vérifié aussi côté overlay : `[hud] HUD affiche` n'apparaît
**jamais**.

C'est cohérent avec l'autre mesure : **zéro appel D3D du Dashboard** en mode XAM. Les deux couches
de rendu sont muettes. La cause est donc en amont des deux, dans le démarrage du XUI.

**Ce que cela écarte définitivement** : « le HUD est cassé », « les shaders du HUD manquent »,
« HudComposite ne compose pas ». Aucune de ces pistes n'est la bonne : le HUD n'est simplement
jamais mis au travail.

### Une boucle d'attente côté XUI (mesurée)

Un journal du mode XAM contient **682 506** appels à `XamLoaderGetMediaInfo` en 38 s, **tous du même
thread `t68`**, qui ne fait rien d'autre. En mode HLE, le maximum observé est **2**.

| | appels `XamLoaderGetMediaInfo` dans une session |
|---|---|
| mode HLE | 2 |
| mode XAM | **682 506** |

Le hook répond correctement (`type=0`, succès) : la boucle est donc **côté code invité**. Le XUI du
Dashboard interroge le média en attendant un état qui n'arrive pas, et n'atteint jamais la suite.

Deux conséquences pratiques :
- le thread est saturé, et le journal grossit de 682 k lignes (il ralentit tout le reste) ;
- c'est un **symptôme mesurable** : si un correctif fait tomber ce compteur à quelques unités,
  c'est que le blocage est levé.

### Le blocage probable : la boîte de message redirigée

Juste après le chargement de `xam.xex`, le journal montre :

```
[xamredir] XamShowMessageBoxUI (ordinal 714) -> XAM recompilé
[xamredir] XamShowMessageBoxUIEx (ordinal 732) -> XAM recompilé
```

`XamShowMessageBoxUI` est **bloquante** : elle ne rend la main qu'une fois l'utilisateur ayant
choisi un bouton. Depuis le palier N5 elle est redirigée vers XAM recompilé, qui n'affiche rien.
Le Dashboard attend donc une réponse qui ne viendra jamais, et n'appelle plus jamais son propre XUI —
d'où l'absence totale d'appels D3D.

C'est cohérent avec le fait que le socle N5 produise lui aussi du noir (`runs/xam_msgbox_t*.png`).

**Piste à tester** : ne pas rediriger `XamShowMessageBoxUI` tant que XAM n'affiche pas, et laisser le
HLE répondre — c'est ce qui fonctionne en mode HLE. Le Dashboard reprendrait la main, et on verrait
si l'écran s'éclaire.

### Dépendances de clés rencontrées au passage

Trois appels de clés tombent sur des stubs :

```
__imp__XeKeysExSetKey STUB            3 fois
__imp__XeKeysGetKeyProperties STUB
__imp__XeKeysExGetKey STUB
__imp__XeKeysExSaveKeyVault STUB
__imp__XeKeysSetProtectedFlag STUB
```

Et `\\Device\\Harddisk0\\Partition1\\secexp.xex` est demandé puis absent (fichier de la flash,
non fourni). À traiter **sans fabriquer de clé de console** : le projet s'en tient à des clés
locales.

## Analyse gdb : les threads invités attendent des objets noyau

Mesure faite sur le processus **sain** (59 threads, état `S`, 6 s après le lancement, donc sans la
perturbation qu'introduit un attach tardif — un attach sur un processus déjà mourant le fait
planter et on ne lit que des piles `do_exit`).

### Ce que montrent les piles

Presque tous les threads invités sont dans le **même état** :

```
pthread_cond_wait
  rex::thread::PosixConditionBase::WaitUntil
    rex::system::XObject::Wait
      rex::kernel::xboxkrnl::xeKeWaitForSingleObject
```

Un seul attend via `xeNtWaitForMultipleObjectsEx` (`XObject::WaitMultiple`). Aucun n'exécute de
code recompilé : ils sont **tous** dans l'attente d'un objet noyau.

Le thread principal, lui, est dans `SDL_WaitEventTimeout` — état **normal** de la boucle X11, pas
un blocage.

### Ce que ça signifie

XAM a créé ses threads et chacun attend un événement, un mutant ou un sémaphore que **rien ne
signalera** : le code qui les signalerait est celui de XAM, et il est lui-même en attente. C'est un
interblocage d'initialisation, pas une fonction manquante.

C'est cohérent avec tout ce qui précède : implémenter cinq stubs n'a rien débloqué, parce que le
problème n'est pas « une fonction renvoie la mauvaise valeur » mais « des objets de synchronisation
ne sont jamais signalés ».

### Fausses pistes écartées par la mesure

| Piste | Pourquoi elle est écartée |
|---|---|
| `null_gpu.cpp` (le « GPU nul ») | ce n'est pas un repli dégradé : c'est le fil qui publie en continu le compteur de fence pour que le D3D invité **ne bloque jamais**. Il fait son travail. |
| thread principal bloqué | il attend X11, ce qui est son état normal |
| stubs noyau | cinq implémentés, aucun ne débloque (les stubs disparaissent des appels, l'écran reste noir) |

### Où chercher maintenant

Il faut identifier **quel objet** chaque thread attend, et qui devrait le signaler. Deux moyens :

1. **`XObject::Wait` instrumenté** : journaliser le handle et le type d'objet (événement, mutant,
   sémaphore) à chaque attente bloquante, avec le thread. Quelques lignes dans le SDK, et la carte
   des dépendances apparaît.
2. **Comparer avec `signin.xex`** : la doc indique qu'il quitte après `XamInputGetState = 0x48F`
   (périphérique non connecté). Vérifier si l'un des objets attendus est celui que `signin`
   signalerait normalement.

La piste 1 est la plus directe : c'est de l'instrumentation, pas de la devinette.

## Ce qu'il faudrait pour aboutir

1. **Instrumenter le blocage** : `gdb` sur les threads invités au moment où le journal s'arrête,
   pour savoir lequel attend quoi. Le thread principal n'est pas le coupable (vérifié).
2. **`HalSendSMCMessage`** (5 appels) : message SMC sans donnée sensible, un succès suffirait
   peut-être.
3. **`EtxProducerRegister`** (15 appels, télémétrie) : un simple succès suffirait probablement.
4. **`XeKeysGetStatus`** : renvoyer « clés locales prêtes », sans clé de console.
5. Les `Nic*` / `Mtpd*` (réseau) peuvent rester en échec propre : XAM est hors ligne.

## État

- **Compilation** : 8 modules `.so`, `dash` lié.
- **Chargement** : XAM chargé, `hud.xex` et `ximecore.xex` chargés.
- **Fautes / FATAL / fichiers manquants** : 0 / 0 / 0.
- **Tables de dispatch** : 4 sur 9.
- **Affichage** : noir — le Dashboard cesse de progresser après l'initialisation de XAM.
- **Version 2.0.13604 sans XAM** : intacte, compile et fonctionne (vérifié `RC=0`).
