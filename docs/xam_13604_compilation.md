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
