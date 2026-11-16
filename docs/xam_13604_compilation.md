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

## Le blocage restant : écriture invité non mappée

Le Dashboard s'affiche **noir** et le journal se termine par :

```
[xinputd] rappel d'arrivée de XAM posé après 0 ms
Unhandled guest access violation: write of guest 0x301A0000 (host 0x00000001301A0000) on thread 0x16
```

`0x301A0000` n'est pas dans la plage d'image de XAM (`0x817E0000 + 0x570000 = 0x81D50000`) : c'est
un accès à un **tas invité** pendant l'initialisation. Aucun `FATAL`, aucun `KeBugCheckEx` : le
processus survit, mais l'écran n'est plus dessiné.

Pistes, dans l'ordre :

1. Tracer l'instruction fautive (hook sur la plage `0x301A0000`, ou `gdb` sur le thread 0x16) :
   savoir si c'est une allocation de tas pas encore faite, ou un pointeur lu avant initialisation.
2. Vérifier `REX_ALIAS_XEX_HEAPS` : le projet signale que les tas `0x8…` et `0x9…` sont deux vues
   du même fichier ; `0x3…` peut relever d'un autre mode.
3. Comparer avec le palier N4 documenté dans `docs/xam_recompile.md`, qui décrivait un parcours
   fonctionnel : voir quel réglage `DASH_XAM_STATE` ou quelle variable y était utilisée.

## État

- **Compilation** : complète, 8 modules `.so` produits, `dash` lié (168/168).
- **Chargement** : `module xam.xex recompilé : chargé`, redirections actives.
- **Affichage** : noir, à cause de l'écriture à `0x301A0000`.
- **Non commité** : rien de ce travail n'est dans le dépôt ; seul ce document l'est.
