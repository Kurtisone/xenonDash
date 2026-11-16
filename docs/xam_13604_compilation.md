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

## Trois obstacles levés, et la piste du HUD écartée

### Levé 1 : le gestionnaire de faute du SDK n'acceptait que les heaps physiques

`Common/src/kernel_xinputd.cpp` écrit `base[port] = 0xFF` dans une allocation `SystemHeapAlloc`.
Or `SystemHeapAlloc` alloue dans `v00000000` (`kGuestVirtual`), et `Memory::AccessViolationCallback`
refusait tout heap autre que `kGuestPhysical` :

```
Unhandled guest access violation: write of guest 0x3019D000 on thread 0x16
```

Seul le bloc `TriggerCallbacks` est spécifique aux heaps physiques (ils portent l'observateur
d'écriture) ; le chemin de récupération (`QueryProtect` / `QueryRegionInfo` / `Protect`) est
générique. Correction dans `third_party/rexglue-sdk`, branche `module-tables`, commit `7348c20`.

**Mesure : violations 1 → 0.**

### Levé 2 : le registre des modules doit porter `flash/`

Les modules XUI sont chargés par XAM sous `game:\flash\<nom>.xex`, que `NormalizeGuestPath`
réduit à `flash/<nom>.xex`. Le registre généré portait `"<nom>.xex"` : `FindRecompiledModule`
échouait, donc aucune table de dispatch, donc `Execute(…): function not in function table`.

**Mesure : tables de dispatch 2 → 4 (`dash`, `xam`, `ximecore`, `hud`) ; `function not in
function table` : 0.**

### Levé 3 : `huduiskin.xex` manquant

`File not found: \SystemRoot\huduiskin.xex`. Fourni par le xeBuild-Data 13604, copié dans
`assets/flash/` (chargé comme ressource, pas comme module recompilé : rien à déclarer).

### Piste du HUD / du focus : écartée par la mesure

`docs/xam_recompile.md` (ligne ~996) proposait : « lancer la boucle d'entrée du HUD à la première
arrivée de la manette indépendamment du focus ». **Ce n'est pas le blocage actuel.** Mesures :

| Vérification | Résultat |
|---|---|
| appels `XInputdReadState` | **0** — le cache d'état n'est jamais sollicité |
| `XexLoadImage("signin.xex")` | **0** — XAM ne charge pas le module de connexion |
| modules chargés par XAM | `hud.xex`, `ximecore.xex` seulement |
| **lignes `[ngpu]`** | **0** — le rendu natif ne s'initialise jamais |
| appels D3D du Dashboard | aucun (les occurrences de « Device » sont des montages VFS) |
| images présentées | 4, puis plus rien |

Autrement dit : le Dashboard n'atteint **jamais son premier dessin**, donc `EnsureCore()` de
`native_gpu/src/front_profile.cpp` n'est jamais appelé, donc aucun device Vulkan n'est créé.
`EnsureCore` journalise `NGPU_ERR` en cas d'échec et ce message n'apparaît pas : il n'est pas
appelé du tout, ce n'est pas un échec d'initialisation.

Le rendu natif est pourtant bien lié au binaire (`noyau ngpu_core` ×2 dans les chaînes, aucune
référence à `librexgpu-xenos`), et `DASH_NATIVE_GPU=ON` dans le cache CMake.

### Où chercher maintenant

La différence avec la version qui fonctionne est visible à l'identique dans les deux journaux,
juste après le montage de la flash :

```
--- 2.0.13604 (fonctionne) ---          --- 2.0.13604-xam (écran noir) ---
\SystemRoot monté sur …/flash          \SystemRoot monté sur …/flash
\Device\Flash monté                  \Device\Flash monté
[rc] Arm() appele, enabled=false       \Device\Harddisk0\Partition1 monté
XConfig category 0x0007                \Device\MU0 monté
XamLoaderGetLaunchData : appel 1       XboxHardwareInfo.Flags = 0x220
                                       UsbdBootEnumerationDoneEvent (0x30000254)
                                       XEX image loaded at 817E0000-81D50000   <- XAM
```

Dans la variante `-xam`, **XAM est chargé sur le thread principal `t12`**, juste après le montage
et avant tout dessin. Dans la version de référence, le Dashboard part immédiatement en
initialisation graphique.

`DASH_XAM_MODULE=1` (chargement seul) et `=2` (chargement + point d'entrée DllMain) donnent le
même résultat : `ngpu=0` dans les deux cas. Le blocage n'est donc pas le `DllMain`, mais le fait
que le chargement de XAM intervienne dans `OnPostSetup`, **avant** la boucle de rendu.

Piste concrète : rendre le chargement de XAM asynchrone (le lancer sur un `XHostThread` après le
premier dessin du Dashboard), plutôt que synchrone dans `OnPostSetup`. À l'inverse, vérifier si
`hud.xex` et `ximecore.xex` sont chargés *pendant* `OnPostSetup` et bloquent la suite.

## État

- **Compilation** : 8 modules `.so`, `dash` lié.
- **Chargement** : `module xam.xex recompilé : chargé` ; `hud.xex` et `ximecore.xex` chargés.
- **Fautes invitées** : 0. **FATAL** : 0. **Fichiers manquants** : 0.
- **Tables de dispatch** : 4 sur 9.
- **Affichage** : noir, car le premier dessin du Dashboard n'a jamais lieu.
