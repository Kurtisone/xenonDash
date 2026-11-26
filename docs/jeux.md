# Jeux : bibliothèque, recettes, recompilation à la demande

Le Dashboard Kinect 2.0.13604 reconnaît les jeux d'un dossier, les recompile en natif au premier lancement (cache), les lance avec le rendu natif `ngpu_core`, puis revient au Dashboard.
Zéro émulation (ni plugin GPU `xenos`, ni Xenia) : un jeu que le mode générique ne sait pas rendre est refusé avec un diagnostic, jamais basculé sur l'émulation.
Le dépôt ne contient aucun fichier de jeu ou de Microsoft, shader, code généré ou binaire : seulement du code, des adresses, des paramètres et des masques de signatures.
Licences : code de Skate repris de Skate1Native (GPL-3.0) ; voir `ngpu_core/THIRD_PARTY.md`.

## 1. Parcours utilisateur

1. Lancer `out/appimage/XenonDash-Kinect.AppImage`. `AppRun` crée le dossier de jeux `~/XenonDash/Games` (ou `$XENONDASH_GAMES_DIR`) et l'annonce ; `AppRun --games-dir` l'affiche.
2. Y déposer ISO, dossiers extraits (avec `default.xex`), paquets XBLA (STFS), GOD, liens symboliques.
3. À chaque démarrage : `game_library.py init` puis `scan` (cache incrémental ; au-delà de 4 s le scan continue en arrière-plan). **F6** relance un scan, **F5** ouvre la liste des jeux
   (A lance). Le scan au démarrage n'est actif que sous AppImage (`XENONDASH_AUTOSCAN=1`, posé par `AppRun`) ou avec `--dash_library_scan`.
4. Le jeu apparaît dans My Xbox (« Play <jeu> », « Latest Games ») et Game Library > Recent Games, avec nom et icône.
5. A sur la tuile, ou Game Library > <jeu> > Play Game, ou Y dans Recent Games : sans binaire en cache, recompilation en processus détaché suivie par `progress.json`
   (étape *n / 8*) ; à la fin le jeu se lance seul, et au retour `$APPIMAGE` est relancé.
6. Refus (`supported=false` + `reason` de `recipe_build.py launch-info`) ou échec (espace disque, `clang`/`cmake`/`ninja` absents) : message, Dashboard utilisable, les imports de lancement renvoient `E_FAIL`.

| Fichier | Rôle |
|---|---|
| `common/src/dash_launch.cpp` | registre fusionné, `LaunchGame`, `Resolve()`, `XamLoaderGetMediaInfoEx`, suivi de construction (`BuildStatePath`, `ResumeBuild`) |
| `common/src/dash_content.cpp` | énumérateur de contenu agrégé (éléments de 512 octets) |
| `common/src/dash_achievements.cpp` | `AllTitles()` : jeux de la bibliothèque ajoutés à la liste des jeux joués |
| `common/src/dash_xam_hud_ui.cpp` | boîte de message Xbox (démarrage et échec de construction), palier N5 de `docs/xam_recompile.md` |
| `tools/xcontainer.py`, `game_library.py`, `recipe_build.py` | formats, inventaire, moteur de recompilation |

Registre : `Games()` charge `<game_data_root>/games.toml` (manuel, **prioritaire** : un TitleID déjà vu n'est pas écrasé) puis `games.auto.toml` (`--dash_library_toml`, `$XENONDASH_LIBRARY_TOML`,
défaut `${XDG_DATA_HOME:-~/.local/share}/xenondash/library/games.auto.toml`), mis en cache par date et taille. Jeux jamais lancés : `AllTitles()` ajoute en mémoire ceux absents de `FFFE07D1.gpd`
(0 succès, icône via `XamReadTile(Ex)` type 1) ; dernière date de lancement dans `<profil>/library_played.txt` (`TitleID FILETIME`). Rien n'est écrit dans les GPD.

## 2. « Play Game » dans le hub 13604

- **Cause racine (corrigée)** : `XamLoaderGetMediaInfoEx` était un stub du SDK. Le hub l'appelle (`sub_92236178`, `sub_922447F8`, `sub_92244748`, `sub_923133E0`) avec trois sorties avant toute action
  et abandonne en silence sur des valeurs de pile aléatoires (il compare la 3e à 0 et à -2). Correctif dans `dash_launch.cpp` : a = type de média (1 = disque de jeu, 0 sans disque), b = TitleID, c = 0 ;
  retour 0. Sémantique déduite des comparaisons du hub, non documentée ; -2 faisait afficher « aucun jeu ».
- A sur « Play Skate » ou Play Game appelle `XamLoaderLaunchTitleEx(NULL, 0x12)` (NULL = « lancer le disque ») que `Resolve()` associe au jeu -> `LaunchGame`. Les trois imports de lancement renvoient `E_FAIL` s'ils reviennent.
- **Liste Game Library** : le hub ne liste un jeu que s'il a un enregistrement de titre, créé par `CLoaderTask` pour chaque élément de `XamContentAggregateCreateEnumerator` dont le type est accepté
  (0x4000, 0x5000, 0x7000, 0x80000, 0xD0000) et dont les bits 0 et 1 du mot à +506 sont nuls. Il lit 512 octets par élément (l'ancien énumérateur n'en écrivait que 0x150) : `dash_content.cpp` les remplit à zéro.
  Tous les jeux restent « Arcade Game » (0xD0000) : le type 0x7000 faisait disparaître les icônes de Skate, cause non établie.
- **Relance du Dashboard** : le hub prend `XamLoaderLaunchTitle*` pour un lancement réel et reste noir pendant la construction, quel que soit le retour du hook. Pour un appel du hub, `LaunchGame` relance donc le Dashboard
  tout de suite et passe l'état par `~/.local/share/xenondash/current_build.json` (pid, cache, ou refus à afficher une fois) ; la nouvelle instance reprend le suivi (`ResumeBuild`) et lance le jeu. F5 garde le suivi dans le même processus.
- Avec le rendu natif, les fenêtres ImGui (progression, F5, bibliothèque) ne sont plus dessinées : remplacées par la boîte de message Xbox. La simulation de disque (`dash_auto_disc`) est supprimée.
- Investigation : gdb en lot, point d'arrêt non bloquant sur les 353 imports `__imp__X*` de `dash.xex` (`dash_init.cpp`), journal de `r3..r6`.

## 3. Bibliothèque : formats (reconnus par le contenu, pas par l'extension)

| Format | Reconnaissance | Remarques |
|---|---|---|
| `dir` | dossier contenant `default.xex` (casse libre) | rien à extraire |
| `xex` | magie `XEX2` | rien à extraire ; le dossier doit contenir les données |
| `xdvdfs` | `MICROSOFT*XBOX*MEDIA` au secteur 32 aux décalages 0, `0x0FD90000` (XGD2), `0x02080000` (XGD3), `0x18300000` (XGD1), puis `0xFB20`, `0x20600` (Xbox d'origine) | lecture ciblée, jamais d'ISO lue en entier |
| `stfs` | magie `LIVE`, `PIRS` ou `CON ` + descripteur STFS | XBLA, démos, mises à jour, DLC |
| `god` | en-tête `LIVE`/`PIRS` à descripteur SVOD (octet `0x3A9` = 1) + `<en-tête>.data/Data0000...` | flux XDVDFS reconstruit sans les blocs de hachage |
| `tree` | dossier sans `default.xex` contenant des paquets (`<TitleID>/<type>/<paquet>`) | récursif |

STFS : table de hachage tous les 170 blocs (L0), 28 900 (L1), 4 913 000 (L2) ; 1 bloc par table pour LIVE/PIRS (bit 0 de `0x37B`), 2 pour CON ; extraction refuse chemins absolus et `..`.
SVOD : partie `DataNNNN` = 1 bloc de hachage maître + 203 sous-parties de (1 + 204 blocs de 4096 octets) ; volume XDVDFS au secteur 32 du flux reconstruit, décalages `0x2000`, `0x1000`, puis la liste ISO.
Identité : en-tête XEX2 (TitleID, MediaID, version) ; nom et icône : ressource XDBF du XEX (chaîne et image `0x8000`), image décodée par `rexglue dump-xex` (`$XD_SDK/bin/rexglue` ou `$XD_REXGLUE`),
à défaut `tools/xexinfo/build/xexinfo` (`$XD_XEXINFO`), à défaut en-tête STFS/GOD, sinon `Titre <TitleID>` sans icône.

```
xcontainer.py info <chemin> [--fast] | list <chemin> [--json] | cat <chemin> <fichier> | extract <chemin> <dest>   (extract idempotente : marqueur .extracted.ok)
game_library.py init [--dir D]   dossier (défaut $XENONDASH_GAMES_DIR, sinon ~/XenonDash/Games) + LISEZMOI.txt
game_library.py scan [--dir D] [--out O] [--force] [--json]   |   list [--out O] [--json]
```

Scan : sortie dans `${XDG_DATA_HOME:-~/.local/share}/xenondash/library/` (ou `--out`) ; récursif, profondeur 8, liens suivis, fichiers cachés ignorés. **Un jeu = un TitleID** : version la plus haute, puis format
(`dir`, `xex`, `xdvdfs`/`god`, `stfs`) ; les autres sources vont dans `alternates`. Hors jeux (`not_games`) : sauvegardes, DLC, profils, thèmes, mises à jour (`0xB0000`, `default.xexp`), Xbox d'origine
(`default.xbe`), module XEX, TitleID `FFFExxxx`. Illisible -> `errors`, non reconnu -> `unrecognized`, sans échec du scan. Cache par signature (chemin, taille, mtime) : 144 sources en 1,3 s à froid, 0,14 s en cache.
Produits : `games.auto.toml` (format de `games.toml`, voir `games.example.toml`), `library.json` (`schema` 1, `games`, `not_games`, `errors`, `unrecognized`, `sources` = cache),
`icons/<TitleID>.png` (copie telle quelle, `icon_oversize` au-delà de 16 Ko), `icons/.cache/`.

```toml
library = true
[game.45410813]
name = "skate."
recipe = "/chemin/absolu/source"   # dossier, default.xex, ISO, STFS ou en-tête GOD (recipe_build.py appelle xcontainer.py extract)
type = "arcade"                    # arcade (défaut), demo (0x80000), xna (0xE0000)
icon = "/chemin/absolu/icons/45410813.png"   # + format, media_id, version : informatifs
```

Limites : ISO, GOD et STFS CON validés seulement sur fichiers synthétiques (STFS LIVE/PIRS sur un paquet réel) ; décalage SVOD `0x2000` non vérifié ; pas de déchiffrement, de multi-disques
(autres disques dans `alternates`) ni d'ISO fragmentées (`.iso.0`) ; Xbox d'origine signalées, non lancées.

## 4. Recettes `games/<TitleID>/` et moteur `tools/recipe_build.py`

Exemple, Skate (`games/45410813/`) : `recipe.toml` (identité, verrou XEX, build, amorçage), `functions.toml` (bornes de fonctions, setjmp/longjmp), `runtime.toml` (réglages d'exécution),
`d3d_profile.toml` (périphérique, adresses D3D, shaders d'interface, GPU nul, constantes), `src/` (hooks propres repris de Skate1Native), `native/` (façade `front.cpp`, `null_gpu.cpp`), `tools/` (récolte des shaders des archives `.big`).
`recipe.toml` (schéma 1) : `[[xex]]` `version`, `media_id`, `sha256_image` (**verrou** : seul un XEX dont l'image décodée a ce hachage est recompilé) ; `[codegen]`, `[shaders]`, `[render] mode = "native"`,
`[build]` (`min_ram_gb`, `min_disk_gb`, `arch_flags`, `estimate_minutes`), `[bootstrap]`. `d3d_profile.toml` (`[device]`, `[functions]`, `[constants]`, `[null_gpu]`, `[profile]`, `[runtime]`) devient
`d3d_profile.generated.h` et `d3d_hooks.generated.inc` (un `REX_HOOK_RAW` par `[[hook]]`).

```
tools/recipe_build.py identify|build|status|launch-info|clean <source> [--generic]    (build : --force, --stages a,b, --jobs N)
```

Étapes : `identify` (XEX, `rexglue dump-xex`, SHA-256, verrou) -> `prepare` (disque, RAM, outils) -> `extract` -> `scaffold` (gabarit `tools/templates/game/`) -> `codegen` (`rexglue codegen`) -> `shaders` (récolte + XenosRecomp,
`shader_cache.cpp`) -> `compile` (cmake + ninja, clang hôte, `-march=x86-64-v2`, tâches = min(cœurs, RAM / 2,2 Go)) -> `bootstrap` (Xvfb + lavapipe : noyau et première image native). `--container` non implémenté.
Cache : `$XDG_DATA_HOME/xenondash/games/<TitleID>/<clé>/` (`project/`, `shaders/`, `bin/`, `build.ok`, `progress.json`, journaux `*.log`, `stamps/`) ; clé = SHA-256 de (image XEX, schéma, SDK, XenosRecomp,
`[codegen]` + fichiers de codegen). Changer hooks ou façade ne refait ni codegen ni shaders. `recipe_build.py clean <TitleID>` libère tout.
Skate : clé `d86199b86b4ee9d2`, image décodée `5b7c7cf4a9c2dafe55b13aaa1d91b5930aafbb94b5effc852ed8ac3f84196fb9` (version 0.0.0.3, MediaID `21D6D331`), 348 shaders, `shader_cache.cpp`
`06880cfb8e8c044d1bab59f16db8544f35a0b1f786d0b7edcbab4e2f89e59553` (identique à Skate1Native) ; cache ≈ 600 Mo, build à froid 12 min sans charge (codegen 124 s, shaders 197 s, compilation 357 s, amorçage 43 s),
20 à 70 s après un changement de hooks, pic RAM ≈ 5,8 Go, 40 à 50 images/s sous lavapipe.

Rendu : `native/front.cpp` porte la façade de Skate1Native (commit `81805f51`) sur `native_gpu::Core` (registre de shaders par clé de microcode XXH3, `StaticShaderProvider`, appels D3D rendus au `Swap` par un fil à part,
`native_gpu_aspect`, `native_gpu_render_scale`), sans plugin GPU, présentation par `native_gpu/src/vk_backend.cpp`.
**GPU nul** (`native_gpu/src/null_gpu.cpp`) : un fil publie « tout consommé » (`*(dev+10896)` = valeur `dev+10908`), un fil de vblank appelle le gestionnaire à 60 Hz. Même code dans les D3D de Skate (XDK 5632) et du 6770 :

| Rôle | Dashboard 6770 | Skate |
|---|---|---|
| attente de fence | `9212A418` | `82CB11C0` |
| drainage de l'anneau (+60) | `92129788` | `82CB0060` |
| fence secondaire (+4) | `921296F0` | `82CAFF88` |
| gestionnaire de vblank | `92121210` | `82CCB758` |

Décalages `+10896`/`+10908` identiques ; plus loin ils diffèrent (14884 contre 14876), d'où la table par jeu.

## 5. Recompilateur générique (XEX sans recette)

`build` prend la recette si `games/<TitleID>/` verrouille ce XEX, sinon le mode générique (`games/_generic/` ; `--generic` ou `XD_GENERIC=1` ignore la recette ; `XD_MIN_DISK_GB` abaisse le seuil d'espace).
`scaffold` (`tools/d3d_match.py` -> profil et crochets dans `gen/`), `codegen` (`tools/codegen_loop.py`), `shaders` (`games/_generic/tools/scan_shaders.py`), `compile` (stubs pour les imports du noyau inconnus), `bootstrap`.
`identify` renvoie `mode` (`recipe`|`generic`), `supported`, `reason`, `xdk`, `static_libraries`, `d3d` (`found`, `missing`, `weak`, `offsets`, `layout`, `optional_missing`, `offset_sources`), `extra_modules`, `generic_supported`/`generic_reason`.

**`d3d_match.py`** (`match|analyze <xex|image.bin> [--exclude FAMILLE]`, `compare <xex> <d3d_profile.toml>`, `learn`) retrouve les fonctions D3D de la XDK liées statiquement :
- jetons PowerPC lâches (classe d'opcode) et serrés (+ registres) ; signatures `tools/d3d_signatures/*.json` apprises depuis `references.toml` (jetons, constantes, imports du noyau, **champs** = décalages du périphérique lus
  à travers l'alignement, **relations** `after_end`/`after`/`callee_of`) ; candidats par 5-grammes sur `.pdata` + 14 premiers jetons ; score = moyenne lâche/serré, contrôles croisés (`VdSwap` pour Swap), unicité, marge sur le 2e candidat ;
- rôles requis (`REQUIRED_ROLES`, 11) : DrawVertices, DrawIndexedVertices, Clear, Resolve, Swap, CreateVertexShader/CreatePixelShader, VblankHandler, WaitFence, WaitDrain, WaitSecond ; facultatifs (`OPTIONAL_ROLES`) :
  BeginVertices, EndVertices, BeginTiling, GpuBeginShaderConstantF4 (absent de Skate) ; décalages lus : vertex_shader, pixel_shader, index_buffer, fence_ptr, fence_value, drain_ptr_word, second_word, fetch_*, reg_surface_info, reg_copy_dest_base ;
- l'adresse du `D3DDevice` n'est jamais figée : la façade la capture à l'exécution (r3 du premier appel crocheté, validé par deux appels) ;
- **verdict `supported`** : 11 rôles requis à confiance ≥ 0,85 et décalages `vertex_shader`, `pixel_shader`, `index_buffer` + 4 du GPU nul lus. La disposition de l'ombre est copiée dans `layout` et canonisée par `ngpu_core/src/device_layout.h`
  (`xenos_decode.h` ne lit que l'ombre canonique). Une famille `experimental` (`fusion-21256`) refuse le verdict sauf `XD_ALLOW_UNSUPPORTED=1` (diagnostic). Ne pas baisser le seuil pour faire passer un jeu ;
- familles : `skate-5632` (D3D 2.0.5632), `dash-6770` (2.0.6132), `crt-13604`, `crt-17559` (setjmp/longjmp), `xapi-21256` (fibres XAPI de Trials Fusion), `fusion-21256` (2.0.21256, expérimentale). Références : `games/45410813/d3d_profile.toml`,
  `native_gpu/src/front6770.cpp`, `dash_overrides.toml` 13604/17559.

**Façade générique** (`games/_generic/native/front.cpp`, `null_gpu.cpp`, `src/` = `GenericApp`) : pilotée par `gen/d3d_profile.generated.h`, `d3d_hooks.generated.inc`, `d3d_nullgpu_hooks.generated.inc`. Contre Skate : pas d'image élargie ni de
liste de shaders d'interface, échelle 1, Swap généré, disposition par profil (`Layout()`), `GpuBeginShaderConstantF4` seulement si le rôle est sûr. Shader absent du cache : journal, dessin sauté, microcode écrit.

**Codegen** (`codegen_loop.py`) : branchements non résolus, fonctions par pointeur (`find_ptr_funcs.py` + `filter_inside.py`), continuations de funclets, fonctions crochetées, `setjmp`/`longjmp` par signature, stubs `REX_STUB_RETURN`.
`tools/func_discover.py` ajoute les candidats hors `.pdata` : `data` (mot de donnée vers `.text`), `code` (`lis` + `addi|ori`, bruité : 70 vrais pour 1 045 faux, donc filtré), `data-in` ; **jamais un thunk d'import du XEX**
(`img.thunks`) : sinon Skate plantait (13 thunks dont `0x8305822C` devenaient des fonctions, `KeTlsSetValue` appelait un stub renvoyant 0, `sub_82E5B328` et `sub_82E5B468` se rappelaient sans fin).
Options, désactivées par défaut : `XD_CODEGEN_BISECT=1` (rejet par moitié, `max_iter` 14), `XD_BOOT_COLLECT=1` (toutes les adresses manquantes d'un coup), `XD_BOOT_LOOPS` (12). Rejets dans `failure.json`.
**Limite connue** : fonctions appelées par pointeur et invisibles de l'analyse statique. Le runtime s'arrête sur « Call to invalid or unregistered function » ; le bootstrap consigne l'adresse dans
`~/.local/share/xenondash/games/<TitleID>/missing_functions.toml` et demande de relancer (un codegen, ≈ 3 min, par adresse). Sans la liste de Skate1Native : `0x82BEB578`.
**Shaders** : `scan_shaders.py` balaie les fichiers du jeu et l'image (conteneurs Xenos, magie `0x102A11xx`, en-tête 0x24), archives non compressées comprises ; RefPack et zlib ne sont pas décompressés.
Un shader absent du cache est journalisé (`not in the shader cache … draw skipped`) et écrit dans `missing_shaders/*.bin` ; **reconstruire** (`build`) l'ajoute. Le cache dépend de la révision de `d3d_match.py`, des signatures et de `codegen_loop.py`.

XDK testées :

| XEX | XDK (D3D9) | rôles requis ≥ 0,85 | vertex_shader / index_buffer / fence_ptr | verdict |
|---|---|---|---|---|
| Skate | 2.0.5632 | 16/16 | 12688 / 12428 / 10896 | supporté |
| Dashboard 6770 | 2.0.6132.4 | 15/16 (BeginTiling 0,77) | 12688 / 12428 / 10896 | refusé (a sa façade) |
| Dashboard 13604 | 2.0.20158 | 6/16 | 12872 / 12612 / 10896 | refusé |
| Dashboard 17559 | 2.0.17559 | 5/16 | 13072 / 12812 / 11024 | refusé |
| Trials Fusion | 2.0.21256 | 3/16 | 13072 / 12812 / 11024 | refusé |

Adresses retrouvées par le matcher : 6770 depuis Skate + CRT (14/14) : BeginVertices `925CC6B8`, EndVertices `925CCB58`, CreateVertex/PixelShader `9212EE10`/`9212ED00`, SetVertex/PixelShader `9212E7E8`/`9212E4E8`,
SetTexture `92123F20`, Set{Vertex,Pixel}ShaderConstantF `9212E2D8`/`9212E3C0`, Swap `92121950`, VblankHandler `92121210`, WaitFence/Drain/Second `9212A418`/`92129788`/`921296F0`. `setjmp`/`longjmp` : 13604 `9288D320`/`9288CEF0`,
17559 `9291C760`/`9291C330`, Trials Fusion `82BC4F60`/`82BC5520`. Skate : `Swap` `82CCBE98` (équivalent 6770 `92121950`, appelé par l'entrée publique `92121EE8` crochetée par `front6770.cpp`), enveloppes Create* `82CC1078`/`82CC0EE8`,
constructeurs `82CC1038`/`82CC0EB0`. Aux XDK tardives, les décalages ont bougé et les scores (0,7 à 0,97) ne sont pas fiables.
Skate générique pur : 115 154 fonctions (10 476 par pointeur), 177 shaders (la recette : 348, issus des archives RefPack), `D3DDevice captured at 40002200` (comme la recette), première image 1280x720, ≈ 17 min ; recette non régressée.

**Échec connu, Trials Fusion XBLA** (XDK 2.0.21256) : `supported` = false (13 rôles sous 0,85, ombre différente : vertex_shader 13072, index_buffer 12812, vertex_constants_slot -304). Avec `XD_ALLOW_UNSUPPORTED=1` : codegen 58 674 fonctions,
compilation OK, `D3DDevice captured at 40852680`, première image 1024x608 mais **image noire** (`draw without a bound shader in the device shadow … skipped`). Cause non établie ; piste : shaders chargés hors de
`SetVertexShader`/`SetPixelShader` (type `GpuLoadShaders`). Manquent aussi : références D3D 2.0.20158/21256, ombre paramétrable par profil, fibres XAPI à l'exécution, 61 fonctions par pointeur sur 253, XEX secondaire
`UplayBrowser.xex`, Bink, DLC. Sans le diagnostic la construction s'arrête sur le refus. Le Dashboard n'affiche pas encore le `reason`.

## 6. AppImage et test de bout en bout

`tools/make_appimage.sh 2.0.13604.0-xam` produit `out/appimage/XenonDash-Kinect.AppImage` (49,9 Mo ; 19,8 Mo avec `--no-build-tools`, sans recompilation de jeux). Embarqué sous `usr/share/xenondash/` : `tools/*.py`, `d3d_signatures/`, `templates/`,
`xexinfo`, `games/` (sans `tests/`), `ngpu_core/`, `native_gpu/`, `patches/`, `sdk/`, XenosRecomp (avec `shader_common.h`, lu à l'exécution : sans lui SIGSEGV), `libdxcompiler.so`/`libdxil.so` ; listes `TOOLS_DIRS` / `ROOT_DIRS` extensibles.
`AppRun` crée le lien `~/.local/share/xenondash/runtime-<version>` vers `usr/share/xenondash`, exporté comme `XENONDASH_ROOT` (`ToolsRoot()`), d'où `recipe_build.py` déduit sa racine (chemins stables) ; il pose aussi `XD_SDK`,
`XD_XENOSRECOMP`, `LD_LIBRARY_PATH`. Prérequis hôte, non embarqués : python3 3.11+, clang, cmake, ninja, GPU Vulkan, 40 Go libres (exigés par la recette ; Skate en pèse 0,6).
- `dashboards/2.0.13604.0-xam/e2e_jeux.sh` : Xvfb `:94`, lavapipe, HOME temporaire, import du firmware, lien vers le jeu, `taskset` 6 cœurs. Variables : `E2E_GAME` (obligatoire), `E2E_OUT`, `E2E_HOME`, `E2E_DISPLAY`,
  `E2E_CACHE_FROM` (cache copié, sans recompilation), `E2E_CORES`. Le HOME doit être sur un disque (une tmpfs de 16 Go est refusée par le contrôle des 40 Go ; `XD_MIN_DISK_GB` l'abaisse).
- `python3 -m unittest discover -s tools/tests -p 'test_library_*.py'` (`XD_TEST_TMP` : dossier temporaire ; ISO, GOD, STFS synthétiques dans `test_library_formats.py`).
- Référence (Skate en lien) : scan en 2 s, A -> recompilation à froid 25 min sous charge, « LANCEMENT de 45410813 », Skate natif à l'écran, retour au Dashboard, 0 FATAL.

## 7. Reste à faire

- Trials Fusion : shaders absents de l'ombre ; Dashboards 13604 et 17559 : références Clear, VblankHandler (13604) et Swap (17559) manquantes ; fibres XAPI à appliquer.
- Fonctions par pointeur : rendre « fonction non enregistrée » non fatal pour consigner toutes les adresses d'une partie, puis un seul codegen ; analyse du flot pour `lis/addi`.
- Décompression des archives (RefPack, zlib) ; reprise du cache de pipelines ; codegen incrémental ; Skate en façade générique ; recettes Trials (Fusion : module PM4 optionnel) ; `--container`.
- Non vérifié : ISO/GOD/STFS recompilés de bout en bout (seule la source « dossier » l'a été) ; tuile My Xbox et « All Games » ; options données à l'AppImage non rejouées à la relance par `$APPIMAGE`.
- Skate : copie CPU vers le présentateur (1280x720 fixe), gamma 1,16 approximé, MSAA, capture/rejeu `.sk1c` non repris.

Historique détaillé avant condensation : `git show d96c252:docs/nxe_jeux.md`, `git show d96c252:docs/recompilateur_generique.md`, `git show d96c252:docs/recettes.md`, `git show d96c252:docs/bibliotheque.md`.
