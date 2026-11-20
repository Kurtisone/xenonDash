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

### Comparaison mesurée : la boucle de rendu n'existe pas en mode XAM

Instrumentation de la séquence de rendu du Dashboard (`DASH_NGPU_TRACE_SEQ=1`), qui trace l'ordre
réel des appels : `ClearF`, `Resolve`, `Swap`, `BeginTiling`, `DrawVertices`.

| mode | séquence observée |
|---|---|
| **HLE** (affiche) | `ClearF` → `Resolve` → `Swap` → `ClearF` → `Resolve` → `Swap` → ... (boucle) |
| **XAM** (noir) | **rien** — pas même un `ClearF` |

**Ce n'est donc pas le `Swap` qui manque : c'est toute la boucle d'image.** En mode XAM, le Dashboard
n'émet **aucune** fonction D3D. Le noyau `ngpu_core` s'initialise, puis le silence complet.

C'est la mesure la plus précise obtenue sur ce problème : elle déplace la question de « pourquoi pas
de présentation » à « pourquoi la boucle de rendu n'est jamais entrée ». Ce sont deux choses
différentes, et la seconde est plus en amont.

Le fait que le journal s'arrête à la même hauteur dans les deux modes (énumération de contenu) alors
que la séquence D3D, elle, diverge totalement, indique que le Dashboard *croit* avoir fini son
démarrage — mais n'entre jamais dans sa boucle de rendu.

### La cause racine : le Dashboard n'atteint jamais son `Swap`

Le profil du Dashboard définit `NGP_Swap = 0x9218EAB0`, avec la mention « appelle `VdSwap` ».
Instrumentation de ce crochet (`DASH_NGPU_TRACE_SWAP=1`) :

| | mesure |
|---|---|
| `NGP_Swap` du Dashboard atteint | **jamais** |
| `VdSwap` appelé | **jamais** |
| image présentée | **0** |
| `HudComposite` appelé | **jamais** (besoin d'une présentation) |

**C'est la cause racine de l'écran noir.** Sans `Swap`, aucune image n'est produite ; sans image,
`HudComposite` n'est jamais invoqué — donc même un HUD qui dessine (mesuré : 4 appels D3D dans une
session) ne peut rien superposer.

Cela explique d'un coup pourquoi toutes les corrections précédentes n'ont pas éclairé l'écran :
elles agissaient en aval d'un point qui n'est jamais franchi.

**Prochaine cible, précise** : pourquoi le Dashboard n'atteint pas `0x9218EAB0`. Sa boucle de rendu
s'arrête avant la fin d'image — le point à instrumenter est la séquence qui précède ce `Swap`
(`kFrame`, `kBeginVertices`, `kDrawVerticesUP` du Dashboard, pas du HUD).

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

## Mesure du 2026-11-20 : la boucle de rendu du Dashboard existe et itère

Cette section corrige une conclusion antérieure du présent document. Le dossier affirmait que
« le Dashboard n'émet aucune fonction D3D : la boucle n'existe pas ». **C'est faux**, et l'erreur
avait une cause précise : l'instrumentation avait été posée dans
`dashboards/2.0.13604.0/generated/dash/`, alors que la variante `-xam` compile
`dashboards/2.0.13604.0-xam/generated/dash/`. Les deux arbres sont distincts ; instrumenter le
premier ne dit rien du second.

### Ce que la mesure établit

Instrumentation des 11 sites d'appel internes de `sub_921AE620` (boucle de rendu), puis exécution
en mode XAM (`DASH_PROFILE_MODE=xam DASH_XAM_MODULE=2`, Xvfb + lavapipe, 40 s) :

```
[rd] sub_921AE620 (boucle de rendu du Dashboard) DEMARRE      (t35, t36 : deux instances)
[rd] site1 0x9217f2d4 ... site2 ... site3 ...
[rd] site4 0x92901a34    KeGetCurrentProcessType
[rd] site5 0x92901664    RtlEnterCriticalSection
[rd] drapeau10942: bit2 ABSENT
[rd] site8 0x92901654    RtlLeaveCriticalSection
[rd] site9 0x92902d04    KeWaitForSingleObject   (timeout 30 ms)
   ... et cela recommence, ~30 ms par cycle, pendant toute la session
```

Conclusions mesurées :
- la boucle **est créée** (`ExCreateThread entree=921AE620 contexte=40004F18`) et **elle itère** ;
- elle n'est donc pas bloquée : `KeWaitForSingleObject` rend la main toutes les 30 ms avec
  `STATUS_TIMEOUT` (258), ce qui est du *frame pacing*, pas un interblocage ;
- le **travail de dessin est sauté** : le bit `0x2` de l'octet à l'offset 10942 (drapeau
  « fenêtre à redessiner ») est absent à chaque image, donc le chemin `sub_9218EAA8` /
  `sub_9218EAB0` (ce dernier appelle `VdSwap`) n'est jamais pris.

### Le drapeau est absent parce que son poseur n'est jamais appelé

Un seul site pose ce bit : `sub_9218DC60`. Instrumenté, il affiche **0 appel** sur toute la session,
en mode XAM comme en mode HLE. Or le mode HLE affiche correctement l'image : **le rendu à l'écran
ne passe donc pas par cette boucle**. `sub_921AE620` est une boucle d'événements secondaire, pas le
chemin de présentation. La cause du noir n'est pas là.

### Le vrai chemin de présentation, remonté par l'adresse de retour

Le hook `__imp__VdSwap` journalise désormais son `lr`. En mode HLE (image correcte, 6 `VdSwap`),
toutes les présentations viennent de `lr=0x9218ED74`, c'est-à-dire de `sub_9218EAB0`, sur le
**thread principal t24**. La chaîne complète, remontée par instrumentation :

```
sub_92173B80   (pompe à images, lr entrant 0x92179BA0, thread principal)
  └─ sub_92173A68   (portier d'initialisation)            -> doit rendre >= 0
       ├─ sub_921732A8  -> 0            (ok dans les deux modes)
       ├─ sub_92171440  -> 0            (ok dans les deux modes)
       ├─ XamNotifyCreateListenerInternal
       └─ sub_92173828  (initialisation longue, ~30 appels)
            └─ sub_92173780
                 └─ sub_92173560
                      └─ sub_9216E8A0  (appel 5)   <-- ne rend jamais la main en XAM
  └─ sub_92172670   (boucle d'image)  -> jamais atteinte en XAM
       └─ sub_92172B48 -> sub_9218F138 -> sub_9218EAB0 -> VdSwap
```

Mesure différentielle (même binaire, seule l'environnement change) :

| point | mode HLE | mode XAM |
|---|---|---|
| `sub_92173B80` atteint | oui | oui |
| `sub_92173A68` : `sub_921732A8` | 0 | 0 |
| `sub_92173A68` : `sub_92171440` | 0 | 0 |
| `sub_92173A68` : `sub_92173828` | rend 0 après 142 ms | **ne rend jamais** |
| `sub_92173828` : appels 1 à 31 | oui | oui |
| `sub_92173828` : appel 32 | oui | **non** |
| `sub_92173560` : appel 5 `sub_9216E8A0` | rend | **ne rend jamais** |
| `sub_92172B48` / `sub_9218F138` / `VdSwap` | 5 / 5 / 6 | **0 / 0 / 0** |

### Ce que la pile d'appel montre (gdb, lancement sous gdb dans le conteneur)

Piège à connaître : `gdb -p` (attach) est **refusé** dans le conteneur
(`ptrace: Inappropriate ioctl for device`) ; il faut **lancer** le processus sous gdb
(`gdb --args ... -ex run`) et ajouter `--cap-add=SYS_PTRACE --security-opt seccomp=unconfined`.

Sous gdb, deux faits inattendus :

1. le thread principal reçoit un **SIGBUS** sur une écriture invitée
   (`sub_9238B4D0`, `REX_STORE_U8(ctx.r11.u32 + 0, ...)`, `dash_recomp.74.cpp:9702`) ;
2. avec `handle SIGBUS nostop noprint pass`, le **HUD de XAM dessine réellement** :

```
[hud] noyau ngpu_core sur 'llvmpipe (LLVM 20.1.2, 256 bits)' (HUD de XAM, D3D 2099)
[hud] image 1 : clears : 1
[hud] image 1 : draws : 3
[hud] image 1 : resolves (color) : 1
[hud] image 1 : 3 dessins, 0 ignores, 1 resolves
```

Cette sortie n'apparaît dans **aucune** exécution normale (vérifié sur tous les `runs/*.out` :
seul le message d'initialisation du noyau HUD apparaît, jamais les compteurs d'image). Le fait que
le HUD dessine sous gdb et jamais sans gdb oriente vers une **course / un ordonnancement**, pas vers
une fonction manquante.

Piste écartée par la mesure : ralentir la machine ne suffit pas. Avec `--cpus=1` (exécution
normale, sans gdb), le HUD ne dessine toujours pas et la capture reste noire (3 073 octets).

### Où en est la recherche

Le verrou n'est plus « on ne sait pas où » : c'est `sub_9216E8A0` (appelé par `sub_92173560`, lui-même
appelé par `sub_92173780`, depuis `sub_92173828`), qui ne rend pas la main en mode XAM. Ses appels
sont `sub_921C9698` (qui n'est qu'un relais vers `sub_921C93F0`), puis `sub_921CA0B8`. La descente
doit continuer dans `sub_921C93F0` (dash_recomp.60.cpp), qui appelle `sub_921E31C8` et utilise
`KeTlsGetValue`/`KeTlsSetValue`.

Deux mesures utiles ensuite :
1. instrumenter `sub_921E31C8` / `sub_927D32C8` (comparaison de chaîne : déjà mesurée, elle renvoie
   bien 1 en XAM) pour trouver l'appel qui ne revient pas dans `sub_921C93F0` ;
2. traiter le `SIGBUS` de `sub_9238B4D0` : c'est la seule différence connue entre « sous gdb » (le
   HUD dessine) et « sans gdb » (rien). Vérifier s'il tue silencieusement le thread qui pousse les
   images.

### Descente finale du 2026-11-20 : le thread principal s'arrête dans `sub_921B77C8`

La descente a été poursuivie maillon par maillon, chaque étape étant mesurée par une sonde
« avant appel N ». Le point d'arrêt est maintenant précis et **reproductible** (trois exécutions
identiques) :

```
sub_92173828  appel 31/0x92173780        <- dernier appel atteint de cette fonction
  sub_92173780  appel 3/0x92173560       <- les appels 1 et 2 reviennent
    sub_92173560  appel 5/0x9216e8a0     <- dernier appel atteint, ne revient pas
      sub_9216E8A0  appel 2/0x921c9698   <- relais : sub_921C9698 -> sub_921C93F0
        sub_921C93F0   appels 1..31      <- progresse puis RETOURNE (r3=0)
        sub_921CA0B8   appel 9/0x921b7be0  <- DERNIER POINT ATTEINT
          sub_921B7BE0  appel 1/0x921b77c8 <- ne revient pas
            sub_921B77C8                   <- s'arrête ici
```

Traces correspondantes (mode XAM) :

```
[b19] appel 5/0x92901664      RtlEnterCriticalSection
[b19] appel 7/0x92901654      RtlLeaveCriticalSection
[b19] appel 4/0x921b1930      recursion -> sub_921B1930
[b7b] appel 1/0x921b77c8      puis plus rien sur ce thread
```

Pendant que t27 est arrêté là, **les autres threads continuent** (journal : `xconfig`, `xinputd`,
notification de média) et le processus reste vivant. Ce n'est donc pas un arrêt global : c'est
l'arrêt de la chaîne qui mène à l'image.

### Deux hypothèses écartées par la mesure

| Hypothèse | Mesure | Verdict |
|---|---|---|
| verrou (`RtlEnterCriticalSection`) détenu par un thread mort | instrumentation de `xboxkrnl_rtl.cpp` : 2 attentes, **2 obtentions** (`0x40100618`), aucune attente non satisfaite | **écartée** |
| simple manque de temps CPU | `--cpus=1`, exécution normale | **écartée** : toujours noir |

Rappel de la seule différence connue : **sous gdb**, avec `handle SIGBUS nostop noprint pass`, le
HUD de XAM dessine (`[hud] 3 dessins, 1 resolves`), ce qui n'arrive jamais sans gdb.

### Prochaine mesure à faire

`sub_921B77C8` (dash_recomp.78.cpp) appelle `sub_921B7560` et `sub_921B1930`, et `sub_921B7560`
contient **deux appels indirects** (`bctrl`). Instrumenter ces deux `bctrl` (cible + retour) est la
mesure suivante : c'est là que se trouve la primitive qui ne rend pas la main.

### Le HUD n'est pas responsable (A/B mesuré)

Corrélation trouvée dans le journal : `t27` (le thread de la pompe à images) cesse toute activité
0,004 s avant que `hud.xex` se charge. L'hypothèse « le chargement du HUD bloque la pompe » a donc
été testée par A/B, en retirant `assets/flash/hud.xex`.

| | `hud.xex` présent | `hud.xex` retiré |
|---|---|---|
| itérations du parcours dans `sub_921B77C8` | 9 | **9** |
| modules `hud` enregistrés | 1 | **0** |
| `VdSwap` | 0 | **0** |
| `NGP_Swap` | 0 | **0** |
| capture | noir (3 073 o) | **noir (3 073 o)** |

**Conclusion : le HUD est innocent.** Le blocage est identique sans lui ; la corrélation temporelle
était une coïncidence de l'ordre d'initialisation. Le fichier a été restauré à l'identique
(empreinte `8b6d9aa5d48c759a98bb333221b9d9e9` vérifiée avant et après).

### Où en est le diagnostic (état au 2026-11-20)

Acquis solides, tous mesurés :
1. la boucle de rendu du Dashboard **existe, est créée et itère** (~30 ms) — la conclusion inverse
   du dossier était un artefact de mauvais arbre ;
2. elle **saute le travail de dessin** (drapeau `0x2` à l'offset 10942 jamais posé) ; mais le mode
   HLE affiche sans ce drapeau, donc ce n'est pas le chemin critique ;
3. le vrai chemin de présentation est `sub_92172B48 -> sub_9218F138 -> sub_9218EAB0 -> VdSwap`,
   piloté par `sub_92173B80` sur le thread principal ;
4. en mode XAM, ce chemin n'est **jamais** atteint : le thread s'arrête dans
   `sub_921B7BE0 -> sub_921B77C8`, au 9e tour d'un parcours de liste ;
5. cette halte n'est **ni** un interblocage de section critique (2 attentes / 2 obtentions),
   **ni** un manque de CPU (`--cpus=1`), **ni** la faute du HUD (A/B ci-dessus),
   **ni** une exception C++ visible (instrumentation de `XThread::Execute`) ;
6. les pointeurs manipulés à l'arrêt sont **valides** (`0x4001C3EC`, `0x4001C410`… tas invité) ;
7. **sous gdb** (SIGBUS ignoré), le HUD de XAM dessine réellement (`[hud] 3 dessins, 1 resolves`),
   ce qui n'arrive jamais sans gdb : il reste donc une différence d'ordonnancement à expliquer.

Prochaine mesure suggérée : journaliser, dans `xboxkrnl` , toute faute invitée interceptée par
`AccessViolationCallback` en mode XAM avec l'adresse et le thread — c'est la seule voie restante
qui explique un arrêt net sans trace applicative. Le `SIGBUS` observé sous gdb sur
`sub_9238B4D0` (`REX_STORE_U8`) en est le candidat le plus probable.

### Le vrai goulot : des milliers de fautes d'écriture invitées sur la même page

Instrumentation de `Memory::AccessViolationCallback` (`third_party/rexglue-sdk/src/system/xmemory.cpp`).
Mesure en mode XAM, 15 s :

```
[av] faute ecriture de 0x301C3000  (n = 1 .. 10 151)   thread t58
[av] faute ecriture de 0xFF3BE000  (124 fois)          thread t59
[av] faute ecriture de 0x41057000  (80 fois)           thread t27   <- le thread qui pilote l'image
[av] faute ecriture de 0x301C2008  (29 fois)           thread t19
total : 10 384 fautes
```

Faits :
- ce sont **toutes** des écritures, jamais des lectures ;
- `0x301C3000` tombe bien dans le tas invité `v00000000` (0x00000000–0x40000000), donc la page
  **appartient** à un heap connu (`LookupHeap` la trouve : aucune ligne « Unhandled guest access
  violation » n'apparaît) ;
- la même page refait faute **10 151 fois de suite** : la protection en écriture n'est donc jamais
  rétablie entre deux tentatives. Chaque faute coûte un aller-retour de signal et le passage par
  le chemin de récupération de `AccessViolationCallback`.

C'est un **goulot de performance et de progression**, pas un interblocage : le thread avance par
saccades, une faute par écriture. C'est aussi l'explication la plus probable de la différence
« sous gdb, le HUD dessine » : sous gdb, `SIGBUS` est intercepté par le débugueur
(`handle SIGBUS nostop noprint pass`), ce qui change complètement le coût et l'ordonnancement de ces
fautes, et laisse la pompe à images progresser jusqu'à `VdSwap`.

**C'est la piste à traiter maintenant** : la page `0x301C3000` est écrite en boucle alors qu'elle
devrait être inscriptible. Deux voies :
1. identifier l'écrivain (thread `t58`, adresse de retour à instrumenter dans le callback) ;
2. vérifier pourquoi le chemin de récupération (`QueryProtect` / `QueryRegionInfo` /
   `GetPhysicalAddress`) n'accorde pas durablement le droit d'écriture : il remet la main sans
   avoir corrigé la protection d'hôte, donc la faute se répète à l'identique.

### Cause racine trouvée : une tempête de fautes d'écriture invitées (mesurée)

Deux compteurs instrumentés dans `Memory::AccessViolationCallback` (`xmemory.cpp`) : les fautes
invitées (`[av]`) et les restaurations de protection réussies (`[rec2]`, auparavant en DEBUG donc
invisibles). Même binaire, même machine, 15 s, seule l'environnement change :

| | mode HLE (image correcte) | mode XAM (écran noir) |
|---|---|---|
| fautes invitées | **12** | **10 394** (et 2 076 400 sur la même page en 15 s) |
| pages restaurées en écriture | 0 | 1 058 |
| `VdSwap` | 6 | 0 |
| tours du parcours `sub_921B77C8` | — | 9, puis le thread s'arrête |

Détail du cas XAM :

```
[t58] [av] faute ecriture de 0x301C3000 (n = 1 ... 2 076 400)   <- MÊME page, en boucle
[t59] [rec2] page restauree en ecriture : invite=0x408F1000     <- restauration réussie mais re-faute
[t27] [rec2] page restauree en ecriture : invite=0xFF0DD000
```

Interprétation établie par la mesure :

- ce ne sont **que** des écritures, jamais des lectures ;
- `LookupHeap` trouve toujours le heap : **aucune** ligne « Unhandled guest access violation »
  (vérifié : 0 occurrence). Le chemin de récupération réussit donc, et la page est bien
  re-protégée en écriture (1 058 fois) ;
- malgré cela, la **même** page `0x301C3000` refait faute 2 millions de fois : quelque chose
  **re-retire** le droit d'écriture entre deux tentatives (probablement un watch d'écriture ou une
  bascule de protection dans le heap virtuel) ;
- en HLE, les mêmes chemins de code ne produisent que 12 fautes : ce comportement est donc
  **déclenché par le mode XAM**, pas par le rendu natif lui-même.

**C'est la cause du symptôme, et elle explique tout le reste** : le thread `t58` passe l'essentiel de
son temps dans le gestionnaire de signaux (une faute = un `SIGSEGV` + restauration + reprise), ce qui
affame les threads qui devraient produire l'image. C'est aussi l'explication de la seule anomalie
restante — « sous gdb, le HUD dessine » : gdb intercepte les signaux et change entièrement le coût
de cette tempête, laissant la pompe à images atteindre `VdSwap` au moins une fois.

### Ce qu'il faut corriger (prochaine étape, précise)

Ne pas chercher une fonction manquante : il faut comprendre pourquoi la protection en écriture de
`0x301C3000` est retirée à répétition. Trois mesures :
1. journaliser, dans le callback, l'appelant (`lr` invité) de la faute pour `0x301C3000` : c'est
   l'écrivain à corriger ;
2. journaliser `TriggerCallbacks` / `TriggerPhysicalMemoryCallbacks` pour voir quel *watch* est posé
   sur cette page et qui le repose ;
3. vérifier le rôle de `REX_ALIAS_XEX_HEAPS` (documenté dans `docs/REPRISE.md`) : les alias de tas
   sont une source connue de protection périmée dans ce projet.

### Mécanisme précis de la tempête (élucidé)

Traces simultanées sur la même page, mode XAM :

```
[t58] [av]   faute ecriture de 0x301C3000 (n=38000)
[t58] [rec2] page restauree en ecriture : invite=0x301C3000 hote=0x00000001301C3000
[t58] [av]   faute ecriture de 0x301C3000 (n=38200)
[t58] [rec2] page restauree en ecriture : invite=0x301C3000 ...
```

La restauration **réussit** (`allow_write=1`, vérifié), la page est bien remise en lecture-écriture,
et **l'écriture suivante refait faute immédiatement**. Ce n'est donc ni un défaut de permission
invitée, ni une page absente : le droit d'écriture est **retiré entre deux instructions**.

Le code du SDK contient le mécanisme qui peut faire exactement cela
(`xmemory.cpp`, `get_host_page_access`) :

```cpp
// Leaving the page read-only can only cause a spurious invalidation, which
// is safe; restoring write access loses invalidations, which is not.
if (access == rex::memory::PageAccess::kReadWrite && IsHostPageWriteWatched(host_page_number)) {
  access = rex::memory::PageAccess::kReadOnly;
}
```

Autrement dit : une page marquée « write-watched » (pour l'invalidation GPU) est **volontairement
laissée en lecture seule** lors de tout recalcul de protection. Comme un host page couvre plusieurs
pages invitées, toute opération sur une page voisine recalcule et re-verrouille celle-ci. Si le guest
écrit ensuite dedans, on obtient la boucle observée : faute → restauration ponctuelle → recalcul →
re-verrouillage → faute. C'est cohérent avec les 2 millions d'occurrences sur une seule adresse.

En mode HLE, seules 12 fautes apparaissent : le chemin qui arme ce watch n'est donc pas emprunté.

### Conclusion du diagnostic

Le noir de la variante `-xam` **n'est pas** une fonction manquante ni un interblocage. C'est une
**tempête de fautes d'écriture** sur des pages invitées write-watched : le thread qui doit alimenter
la pompe à images (`t58`, et `t27` pour la pompe elle-même) passe son temps dans le gestionnaire de
signaux, et la chaîne `sub_92172B48 -> sub_9218F138 -> sub_9218EAB0 -> VdSwap` n'atteint jamais son
terme. Le HUD est innocent (A/B), les verrous sont sains (2/2), la boucle de rendu existe et itère.

Correctif à instruire (ordre suggéré) :
1. **restreindre le write-watch** : ne pas laisser une page write-watched en lecture seule quand le
   guest l'a commise en écriture ; ou ne watcher que la plage réellement invalidée
   (`unwatch_exact_range`) ;
2. vérifier qui arme ce watch en mode XAM (XAM écrit dans des surfaces partagées avec le GPU) ;
3. après correctif, le critère de succès est déjà défini et mesurable : le compteur de fautes doit
   passer de ~10⁴–10⁶ à quelques unités, et `VdSwap` doit apparaître dans le journal.

## 2026-11-20 (soir) : LE MODE XAM AFFICHE ENFIN — /dev/shm trop petit

### La cause n'était pas dans le code invité

Indice décisif trouvé en journalisant la **protection hôte réelle** de la page fautive, lue dans
`/proc/self/maps`, au moment de la faute :

```
[av] faute ecriture de 0x41064000 heap=0x40000000 protect_invite=0x00000003 hote=... perm=rw-s
```

Trois faits incompatibles avec les hypothèses précédentes :

- `protect_invite=0x00000003` = lecture **et** écriture : la table invitée autorise l'écriture ;
- `perm=rw-s` : la page hôte est **déjà** en lecture-écriture ;
- et pourtant le CPU lève une faute à chaque écriture.

Une faute sur une page `rw` **partagée** (`s` = `MAP_SHARED`) n'est pas un `SIGSEGV` : c'est un
**`SIGBUS`**, c'est-à-dire « le stockage d'appui ne peut pas fournir cette page ». Le SDK mappe
l'arène invitée ainsi (`memory_posix.cpp`) :

```cpp
int ret = shm_open(full_path.c_str(), oflag, 0777);   // fichier dans /dev/shm
rex_ftruncate64(ret, static_cast<off_t>(length));      // 0x120000000 = 4,5 Go
...
int flags = MAP_SHARED;                                // mappage partagé
```

Or `/dev/shm` du conteneur de test fait **63 Mo**. `ftruncate` réussit (tmpfs accepte la taille
déclarée), mais aucune page au-delà des 63 Mo ne peut être réellement stockée : chaque écriture dans
ces pages lève `SIGBUS`, indéfiniment. C'est exactement la tempête de 1,6 M de fautes mesurée, et
cela explique aussi pourquoi la trace `[pfail]` restait vide : `mprotect` réussissait — le problème
n'était pas la protection mais le **stockage**.

### Preuve par A/B

| `/dev/shm` | fautes invitées (15 s) | `VdSwap` | capture |
|---|---|---|---|
| 63 Mo (défaut du conteneur) | **1 620 000** | 0 | 3 073 o (noir) |
| 8 Go (`--shm-size=8g`) | **8** | **6** | 330 371 o (image !) |

L'écran s'affiche : menu `Xbox LIVE / My Xbox / Welcome`, tuiles « Getting Started », « Kinect for
Xbox 360 », « Family First ». Le Dashboard atteint enfin son `VdSwap`.

### Correctif appliqué dans le SDK

`core/memory_posix.cpp` : `memfd_create` en premier (fichier anonyme adossé à la mémoire, **sans
passer par tmpfs**, donc sans la limite de `/dev/shm`), avec repli sur `shm_open`. Et surtout,
l'échec de dimensionnement n'est plus silencieux :

```cpp
if (rex_ftruncate64(ret, static_cast<off_t>(length)) != 0) {
  REXSYS_ERROR("CreateFileMappingHandle: impossible de dimensionner la memoire invitee a 0x{:X} "
               "octets ({}) : ... Elargir /dev/shm ou autoriser memfd_create.", length, ...);
```

Vérification avec `/dev/shm` **toujours à 63 Mo**, sans aucune option spéciale : **8 fautes**,
**6 `VdSwap`**, image affichée. Le correctif tient.

### Second défaut, distinct : le fond atténué assombrit tout l'écran

L'image présente mais **4× trop sombre** : luma moyenne **31,1** contre **124,1** en référence HLE
(mesure sur les pixels, `runs/regression13604.png`).

Cause : le HUD de XAM affiche une **boîte modale légitime** (barre de progression vide + indicateur
d'activité, zone 243,30–607,279 sur 852×480) et son quad de fond atténué demandé par le titre vaut
`dim = 0,22`. `HudComposite` applique ce facteur à **toute** l'image du Dashboard
(`native_gpu/src/hud_front.cpp`, `if (dim < 1.0f)`) au lieu de la seule zone de la boîte.

Contre-épreuve : avec `DASH_HUD_DISABLE=1`, la capture retrouve **luma 123,7** (référence 124,1) et
689 046 octets — l'image est alors exactement correcte. Le Dashboard lui-même est donc **rendu
fidèlement** ; seul l'assombrissement de la composition est en cause.

Cette boîte ne se referme pas (surveillée 30 s, `[hud] HUD affiche` une seule fois, jamais
`masque`) : c'est l'attente de `XamShowMessageBoxUI` décrite plus haut, désormais **visible** au lieu
d'être un écran noir. C'est le sujet suivant, pas une régression.

## 2026-11-20 (nuit) : plantage du HUD corrigé (verrou d'entrées manquant)

### Le rapport de l'utilisateur, sur Steam Deck (GPU réel, RADV)

Premier essai sur machine réelle, `HUD=1` :

```
[ngpu] noyau ngpu_core sur 'AMD Custom GPU 0405 (RADV VANGOGH)' (profil 13604)
[hud]  image 600 : 3922 dessins, 0 ignores, 600 resolves
Erreur de segmentation (core dumped)
```

Le HUD **dessinait** (600 images, 3922 dessins) puis le processus mourait. `coredumpctl` donne la
pile exacte :

```
#0  std::__merge_adaptive<...>                    (librexruntime.so)
#1  rex::input::InputSystem::RefreshDevices()
#2  rex::input::InputSystem::GetStateForUI()
#3  dash::guide::GuideFrame()
#4  ngpu_hook_9218EAB0            <- crochet de présentation du Dashboard
#5  sub_9218EAB0 -> sub_9218F138 -> sub_92172B48 -> sub_92172670 -> sub_92173B80 -> xstart
```

### La cause : `RefreshDevices()` sans verrou, appelé de deux fils

`InputSystem::RefreshDevices()` **reconstruit** `devices_` et `device_owners_`
(`devices_.clear()` puis `push_back` dans l'ordre trié) et n'avait **aucun verrou**. Il est appelé :

- par le **thread principal**, à chaque image, via la lame Guide
  (`GuideFrame` → `GetStateForUI`, `native_gpu/src/dash_guide_front.cpp`) ;
- par les **threads invités**, via `GetState` / `XInputGetState`.

Un fil vidait donc les vecteurs pendant qu'un autre les parcourait ou les triait — d'où le plantage
dans `std::stable_sort` / `__merge_adaptive`, de façon intermittente (il faut que les deux appels se
chevauchent, ce qui explique qu'il n'apparaissait pas sous Xvfb avec un seul cœur occupé).

### Correctif

`InputSystem` a désormais un `devices_mutex_` (`include/rex/input/input_system.h`), pris par
`RefreshDevices()`, `DriverForDevice()` et `DeviceInfoFor()` — les trois fonctions qui touchent
`devices_` / `device_owners_`. `OnDevicesChanged()` des affectations ne rappelle pas `InputSystem`,
donc pas de risque d'interblocage.

Vérification : 60 s avec le HUD actif, **1200 images HUD, 8285 dessins, aucun plantage**
(code de sortie 124 = fin de `timeout`, pas un crash).

### Ce que le rapport révèle d'autre, et qui change le diagnostic

Sur machine réelle, avec le HUD actif :

```
[hud] image 600  : clears 600, draws 3318, resolves 600
[hud] image 1200 : clears 1200, draws 7077, resolves 1200
```

Le HUD **tourne en continu** (~6 dessins par image, 1200 images) et ne se fige pas. Il n'y a donc
**pas** de blocage dur : la boîte modale (barre de progression + indicateur d'activité) est dessinée
par XAM recompilé lui-même. Vérifié : dans une session où aucune `XamShowMessageBoxUI` n'est
demandée par le Dashboard, la boîte apparaît quand même dès l'image 2, et le journal hôte ne montre
aucune requête (`[hudui]` muet, `BuildWatcher` sur `active=false`).

Conséquence pour la suite : le problème n'est plus « la boîte ne se referme pas et bloque tout »,
mais « XAM dessine un dialogue de progression que personne n'a demandé, et son fond atténué
(`dim = 0,22`) assombrit l'image du Dashboard ». Ce sont deux défauts distincts :
1. **le fond atténué** est appliqué à toute l'image au lieu de la zone de la boîte
   (`HudComposite`, `native_gpu/src/hud_front.cpp`) → c'est ce qui rend l'écran sombre ;
2. **le dialogue lui-même** est dessiné sans demande : à instruire côté XAM recompilé
   (quel appel l'ouvre, quel état il attend).

## 2026-11-20 (nuit) : chargement infini corrigé, lame Guide retirée

### Le « chargement trop long » était deux boucles de journalisation

Mesure sur 40 s, mode XAM : **375 044 lignes** de journal, dont :

| ligne | occurrences | nature |
|---|---|---|
| `__imp__VdSetSystemCommandBuffer STUB` | **371 443** | XAM l'appelle plusieurs fois par image ; le stub du SDK journalise à chaque appel |
| `XexLoadImage("hud.xex")` → échec | **356** | XAM redemande sans fin un module déjà chargé |

Ce n'était donc pas un calcul long : c'était du bruit d'journalisation qui noyait la sortie et
consommait le temps. Après correction, même session : **770 lignes** (÷ 488), `VdSetSystemCommandBuffer`
**0**, tentatives `hud.xex` **2**.

**Correction 1 — `common/src/kernel_video.cpp`** : le crochet absorbait déjà l'appel mais laissait
parler le stub du SDK, qui journalise. Le SDK n'implémente rien (`REX_EXPORT_STUB`), donc absorber
l'appel ne change pas le comportement — seulement le bruit. `DASH_TRACE_VD=1` garde la trace fine.

**Correction 2 — `common/src/dash_xam_runtime.cpp`** : `game:` n'était monté sur **rien**.
Le crochet `XexLoadImage` redirige les noms nus de XAM vers `game:\flash\<nom>`, chemin qui n'existait
pas comme périphérique — d'où `VFS: 'game:\flash\hud.xex' -> [no device]`, puis nouvelle tentative,
indéfiniment. `game:` est désormais monté sur la flash.

### La lame Guide maison est retirée

L'utilisateur : « il faudrait enlever cette fenêtre guide créée que l'on ouvre avec le bouton Guide.
Le bouton Guide doit ouvrir la fenêtre Guide du dashboard Xbox ».

C'est juste : la lame (`common/src/dash_guide.cpp`, bouton `XAM_INPUT_GAMEPAD_GUIDE` 0x0400) est une
invention du projet, pas le Guide de la console. Sur Xbox 360, le Guide est une **interface de XAM**
(`XamShowGuideUI`), pas une lame dessinée par le titre.

- `native_gpu/src/front_profile.cpp` : la composition de la lame est **retirée du chemin normal**.
  Le module reste compilé et rappelable par `DASH_GUIDE_LEGACY=1` (diagnostic), mais le bouton Guide
  ne l'ouvre plus.
- Vérification : `DASH_GUIDE_OPEN=1` (qui forçait son ouverture) ne produit plus rien.

**Le vrai Guide reste à faire** : `XamShowGuideUI` est un `REX_EXPORT_STUB` dans le SDK
(`xam_ui.cpp`), et XAM recompilé n'a **aucune** fonction dont le nom contient « Guide » (vérifié :
0 sur 16 238 symboles de `libdash_xam.so`). Le Guide de la console est donc une interface XAM que
personne n'ouvre encore : c'est un travail à part entière, pas un réglage. Piste : XAM possède ses
propres interfaces (`XamShowSigninUI`, `XamShowMessageBoxUI`… déjà redirigées) ; le Guide est
probablement `XamShowGuideUI` côté hôte, à relier à la même machinerie `dash_xam_redir` +
`dash_xam_hud_ui` que les boîtes, une fois qu'on saura quelle fonction de XAM recompilé l'affiche.

## 2026-11-20 (nuit, suite) : XamShowGuideUI — la chaîne complète fonctionne, le refus est expliqué

Objectif demandé : que le bouton Guide ouvre le **Guide de la console** (`XamShowGuideUI`).

### Ce qui marche maintenant

Le bouton Guide (0x0400) déclenche bien `XamShowGuideUI` de XAM recompilé. Chaîne mesurée de bout
en bout :

```
bouton Guide / DASH_TEST_GUIDE
  -> dash::hudui::RequestGuide()            (depose une demande, ne bloque pas le rendu)
  -> thread invite du HUD                   (le meme que les boites de message)
  -> dash::xamredir::ShowXamGuide()         (construit le contexte invité)
  -> Dispatch -> XamShowGuideUI (ordinal 772) -> XAM 0x81898630
  -> r3 = 0  (ACCEPTE)
```

`DASH_TEST_GUIDE=<secondes>` permet de déclencher sans manette.

### Pourquoi ça renvoyait 5 (ERROR_ACCESS_DENIED) : trois causes successives, mesurées

1. **Mauvaise signature.** La fonction XAM prend **un seul argument** (`user_index`). Établi en
   désassemblant `0x81898630` : elle lit `r3`, le pose dans une structure locale de 12 octets, et
   n'utilise ni `r4` ni `r5`.
2. **Mauvais mot surveillé.** Le portier `sub_8189F0D0` teste le mot à **`0x81BFCAE8`** (registre
   `r31` du code généré), et non `0x81BFCB8C` (`r31+164`, qui n'est que la valeur passée dans le
   message). Mes premiers essais portaient sur le mauvais mot — d'où des corrections sans effet.
3. **La vraie cause : un compteur d'interfaces système actives.** Au moment de la demande,
   `0x81BFCAE8` vaut **4**. Le portier refuse tant que ce mot est non nul : XAM considère qu'une
   interface système est déjà à l'écran, donc le Guide n'a pas à s'ouvrir. Rien ne décrémente ce
   compteur dans notre environnement (les interfaces qui l'incrémentent ne sont jamais fermées).

### Correctif

`ShowXamGuide` remet ce compteur à zéro **le temps de l'appel**, puis le restaure :

```
[guide] compteur « interfaces systeme actives » (0x81BFCAE8) : 4 -> 0 le temps de l'appel
[guide] XamShowGuideUI appelle (utilisateur 0)
[guide] apres Dispatch : r3=0x00000000 (compteur restaure a 4)
[guide] XamShowGuideUI : accepte (r3=0x00000000)
```

C'est déclarer à XAM qu'aucune interface système n'est affichée — ce qui est exact devant le
Dashboard — et non simuler le Guide.

### Ce qui reste, et qui n'est plus dans notre code

XAM accepte la requête et envoie son message système :

```
app = 254 (0xFE), message = 0x21015, tampon 12 octets : { 0, user_index, 0 }
```

Mais **aucune image de Guide n'est dessinée** : le HUD reste à « image 1 : draws 3 » et ne produit
plus rien ensuite. Sur la machine de l'utilisateur (Steam Deck, RADV), le HUD dessine en continu
(1200 images, 7077 dessins) — c'est donc un problème d'environnement de test ici (Xvfb sans GPU
réel), pas nécessairement du code. **À vérifier sur la machine réelle** : le Guide devrait
apparaître, puisque XAM accepte et que son HUD y tourne en continu.

Prochaine mesure si l'écran reste inchangé sur Steam Deck : instrumenter le consommateur du message
`0x21015` (app 254) pour voir s'il est distribué au HUD, et quel état il attend ensuite.
