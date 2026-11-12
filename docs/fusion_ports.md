# Fusion des ports de jeux dans XenonDash : inventaire (P8, étape 1)

Date : 2026-11-08. Inventaire en lecture seule de `../Skate1Recompiled` (« Skate1Native », dépôt `Kurtisone/Skate1Recompiled`)
et de `../TrialsReVolution` (« Trials: ReVolution Collection », dépôt `Kurtisone/Trials-ReVolution-Collection`), comparés au fork SDK
de XenonDash (`third_party/rexglue-sdk`, branche `xenondash`) et à `native_gpu/` (P7, [rendu_natif.md](rendu_natif.md)).
Rien n'a été modifié dans les deux projets voisins. Les essais d'application de correctifs ont été faits sur une copie
temporaire du SDK de XenonDash (supprimée). Aucun extrait important de code GPL n'est reproduit : tout est résumé.

## 0. Résumé

- Les trois projets reposent sur **ReXGlue v0.10.0 (`c94f5eb`)**. Le fork XenonDash y ajoute 6 commits (noyau/runtime générique, 9 fichiers),
  Skate vendorise un SDK modifié (**270 fichiers** différents de la 0.10.0 hors `thirdparty/`, ~34 000 lignes de diff), et Trials
  reconstruit le sien avec `tools/setup_sdk.sh` : 0.10.0 + `sdk-patches/linux-android/0001..0008`, dont le 0001 (298 fichiers) **est le
  travail de Skate1Native** (ARM64, Android, Vulkan, crochets de rendu natif). Les 21 correctifs `sdk-patches/0001..0021` sont
  des correctifs D3D12/Vulkan de l'émulation GPU, déjà fusionnés dans `linux-android/0001/0002` pour Linux.
- Test réel : `linux-android/0001..0008` s'appliquent sur le SDK de XenonDash, **sauf `mnk_input_driver.{h,cpp}`** (conflit
  avec notre commit « synthétiser les événements de touche »). Les 6 autres fichiers communs (`kernel_state`, `xmemory`,
  `phase_register`, `xboxkrnl_threading`…) fusionnent. Le SDK unifié est donc faisable en quelques jours, pas en semaines.
- **Licences** : Skate1Native est GPL-3.0 ; Trials n'a **aucune licence déclarée** pour son propre code (pas de `LICENSE`, 46 commits
  de TekRantGaming sur 87) ; XenonDash n'a **aucune licence** non plus. Reprendre Skate impose de publier XenonDash en GPL-3.0 ;
  reprendre Trials impose l'accord écrit de TekRantGaming (ou de n'en reprendre que les parties MIT/de soi).
- **Rendu natif** : le rendu de Skate est une couche D3D par jeu (XDK 2.0.5632, état lu dans l'« ombre » du périphérique D3D), pas
  une bibliothèque générique. Recommandation en §3 : un noyau commun (Vulkan, textures, shaders, cibles, résolutions) et des
  « profils » de façade D3D par version de XDK. Trials n'utilise **pas** le rendu natif par défaut (émulation GPU + 21 correctifs).
- Plan chiffré (§6) : ≈ 62 j ± 30 %, jalon « Skate lancé depuis le Dashboard » vers 37 j.

## 1. Inventaire par projet

### 1.1 Skate1Recompiled / Skate1Native

| Point | Constat |
|---|---|
| Jeu | skate (EA Black Box, 2007), `game/default.xex` extrait d'une ISO par `tools/extract_iso.py` (6,4 Go de `game/`). XDK D3D 2.0.5632 lié statiquement |
| SDK | `third_party/rexglue-sdk` **vendorisé** (pas de sous-module, 1,1 Go avec sa construction), v0.10.0 + travaux de `rexglue-skate3` (mchughalex) + changements propres (historique `Kurtisone/rexglue-skate3`, branche `skate1-dev`, jusqu'à `94cf105`). Aucun correctif séparé : il faut comparer par `diff` à la 0.10.0 |
| Dépôt | ~49 commits, un seul auteur (Kurtisone). Pas de sous-modules : tout est copié dans `third_party/` (`libadrenotools`, `plume`, `smol-v`, `XenosRecomp`, SDK) |
| Génération | `skate1_manifest.toml` (sdk 0.10.0, entrée `game/default.xex`) + `config/skate1_functions.toml` (509 lignes : bornes de fonctions, setjmp/longjmp) ; `rexglue codegen` ≈ **9 min**, sortie `generated/` 266 Mo (`.text` ~50 % plus gros que Skate 2) |
| Exécution | `config/skate1.runtime.toml` (47 lignes : touches, `quit_dialog`, options GPU émulé de repli) |
| Build | `build-linux.sh` : détecte l'ISO (reconnue par la présence de `screens.big`), vérifie **40 Go libres, 16 Go de RAM conseillés**, exige podman ou docker. Image `tools/steamdeck/Containerfile` (Ubuntu 22.04, clang 20, cmake Kitware, Vulkan SDK LunarG, en-têtes X11/Wayland/audio : plancher glibc 2.35 pour le Steam Deck). Cible Android séparée (NDK r29, SDK 35, JDK 17). `out/` pèse **14 Go** ici. Reprise après interruption |
| Vérification du XEX | Aucune par hachage : seule la reconnaissance de l'ISO (`screens.big`) |

**Propre au jeu** : `src/skate1_app.h` (sous-classe `ReXApp`, chemins), `pause_settings.cpp` (page vidéo ajoutée aux options du jeu,
textes préfixés `#`), `quit_menu.cpp` (RB+Start), `skip_intro.cpp`, `scene_tiling.cpp` (réduction du *tiling* prédiqué à une passe),
`frame_cap.cpp`/`perf_log.cpp` (cadence 30/40/60, génériques dans l'idée mais câblés sur les adresses de Skate), élargissement de la caméra
(`sub_8274F1B0`, doubles 16/9 à `0x82191258`/`0x82191174`), `tools/extract_iso.py`, `harvest_shaders.py`/`refpack.py` (shaders
dans des archives `.big` compressées RefPack), `config/skate1_functions.toml`.
**Générique** : couche D3D native (§3), audio, entrées clavier/souris/manette, menu RB+Start (le principe), fenêtre SDL, build
conteneur, Android (`tools/android/`, `libadrenotools`), `tools/native_replay` (rejeu hors ligne), capture d'images.

### 1.2 TrialsReVolution

| Point | Constat |
|---|---|
| Jeux | Trials HD (`5841095A`, XEX 0.0.1.2), Evolution (`58410AAD`, 0.0.0.3), Fusion (`58411401`, 0.0.0.3), jeux XBLA dont le seul `default.xex` est tiré du paquet STFS (`Content/0000000000000000/<TitleID>/000D0000`) |
| Structure | `games/<jeu>/` : `<jeu>_manifest.toml`, `codegen.toml` (setjmp/longjmp, fibres `[rexcrt]`), `functions.toml` (HD 112 lignes, Evo 13, Fusion 19), `missing_funcs.toml` (Evo 171, Fusion 243), `src/` (main, `port_defaults`, `input_options`, `frame_stats`, `draw_distance`, HD : `fps_unlock`, `dof_option`) ; `games/shared/src` (toast de succès, retour au lanceur, modes de qualité, fenêtre, garde FPE, export des succès, limiteur) ; `games/shared/native_gpu` (rendu natif par flux de commandes, 5 266 lignes) ; `src/` = **lanceur** ImGui/SDL (accueil, installation de paquets, DLC, sauvegardes, succès, raccourci Steam) ; `third_party/trg-launcher`, `plume`, `libadrenotools`, `smol-v` ; `tools/` (`setup_sdk.sh`, `trials_extract.cpp`, `native/` shaders) |
| SDK | `tools/setup_sdk.sh` : clone de `rexglue/rexglue-sdk`, `c94f5eb` (v0.10.0), SDL 3.4.14, puis `git apply` de `sdk-patches/linux-android/*.patch` (tampon de hachage pour ne pas ré-appliquer). Windows : un SDK de TekRantGaming (FSR 1, NIS) hors dépôt, plus `sdk-patches/0001..0021` via `git am` |
| Build | `build-linux.sh --packages <dossier>` : conteneur Ubuntu 22.04 (clang 20), **16 Go de RAM, 40 Go par jeu**, « about half an hour » de recompilation par jeu ; `dist/` 149 Mo, `out/` 3,2 Go ici, `games/trials_fusion/generated` 240 Mo. Vérification du jeu par **TitleID** du paquet (message d'erreur « that is another game »), version du XEX attendue affichée (`games.cpp`) mais pas de hachage |
| Rendu | Défaut : **émulation GPU de ReXGlue** (backend Vulkan, `render_target_path_vulkan`) avec les 21 correctifs. En option, rendu natif piloté par le flux de commandes (`native_gpu_render`, Fusion seulement, Turnip T30, dépôt `docs/native_port.md`) : 45 FPS sur la première piste sous RADV, mémoire-export, textures virtuelles, ombres, éclairage corrigés au fil des journaux |
| Vérifié | Fusion : paquet → binaire Linux → piste sur AMD ; Android (Odin 2). HD et Evolution **jamais construits** sous Linux |

**Propre aux jeux** : `codegen.toml`/`functions.toml`/`missing_funcs.toml` (adresses de setjmp/longjmp, fibres, bornes de fonctions),
`port_defaults.cpp` (options GPU par titre, p. ex. `texture_fetch_integer_8bpc` désactivé pour HD), `fps_unlock`/`frame_clock`/`render_pose`
(HD, venus de snufflyyy), `draw_distance`, `dof_option`.
**Générique** : lanceur complet (installation de paquets STFS, DLC, sauvegardes, succès, mise à jour, Steam, manette), `games/shared/src`,
rendu FSR/NIS (Windows), build conteneur, Android.

### 1.3 Fork SDK de XenonDash et `native_gpu/`

- `third_party/rexglue-sdk` : `c94f5eb` + 6 commits (noyau et runtime génériques : table des fonctions dans le tas de l'image, clavier/souris
  synthétisés, arguments u64 noyau/XAM, imports sans thunk ignorés au codegen, chargement de modules `libXXX.so` à côté de l'exécutable,
  DPC `KeInsertQueueDpc` + HMAC-SHA1). Doublés par `patches/rexglue-sdk-000{1,2,3}-*.patch` + `patches/xenosrecomp-dash360.patch`.
- `native_gpu/` (5 026 lignes, option `DASH_NATIVE_GPU`) : écrit sans code d'UnleashedRecomp (MIT/BSD seulement), Vulkan direct, 3 281
  enveloppeurs de fonctions D3D du Dashboard 6770 (symboles forts sur alias faibles `sub_XXXXXXXX`), GPU nul + vblank, **aucune
  modification du SDK** (le mode « détaché » suffit).

## 2. Licences

| Composant | Licence | Fichier |
|---|---|---|
| Skate1Recompiled (code propre) | **GPL-3.0** (« GPL-3.0. skate is a trademark of Electronic Arts »), car il intègre/dérive d'UnleashedRecomp | `COPYING`, `README.md` |
| UnleashedRecomp (`src/native_gpu/unleashed/`, commit `cf829a9`) | GPL-3.0 ; copié pour référence (non compilé : `CMakeLists.txt` ne cite que `renderer.cpp`/`live_renderer.cpp`), mais `renderer.cpp` reprend ses structures (tas bindless, constantes partagées) | `THIRD_PARTY.md` |
| ReXGlue SDK / Xenia | BSD-3 (Tom Clay ; parties Xenia : Ben Vanik et contributeurs) | `LICENSE` du SDK |
| XenosRecomp, XenonRecomp (hedge-dev) | MIT | `third_party/XenosRecomp/LICENSE.md` |
| plume (renderbag), smol-v, SDL, imgui, fmt, spdlog, toml++, simde, xxHash, utfcpp, o1heap, tracy, glslang, spirv-*, Vulkan-* | MIT / zlib / BSD / Boost / Apache-2.0 | `THIRD_PARTY.md`, dossiers |
| FFmpeg (fork wmarti) et libmspack (SDK, `thirdparty/`) | **LGPL** (lien dynamique ou source disponible) | `THIRD_PARTY.md` |
| libadrenotools | voir son `LICENSE` (BSD-style) | `third_party/libadrenotools/LICENSE` |
| Trials (code propre : lanceur, `games/`) | **non déclarée** ; mêmes auteurs : Kurtisone (41 commits) et TekRantGaming (46) | aucun fichier |
| Trials HD Recompiled (snufflyyy) : listes de fonctions, `codegen.toml` de HD, `fps_unlock`, `frame_clock`, `render_pose`, `flat_hash_map`, lecteur STFS | MIT (avis à reproduire) | `THIRD_PARTY_NOTICES.md` |
| TRG Launcher (TekRantGaming) | non déclarée dans le dépôt vendorisé (README seulement) | `third_party/trg-launcher` |
| FSR 1 (AMD), NIS (NVIDIA) | MIT | `README.md` |
| Correctifs ports de Xenia Canary (`0011`) | BSD-3 (même famille que ReXGlue) | `THIRD_PARTY_NOTICES.md` |
| **XenonDash (état actuel)** | **aucune licence** (pas de `LICENSE`/`COPYING`, aucune mention dans `README.md`/`PLAN.md`) | — |

**Conséquences pour XenonDash**
1. Reprendre du code de Skate1Native (rendu natif, menus, hooks) rend XenonDash **GPL-3.0** pour l'ensemble distribué (un binaire qui
   lie du code GPL). Les composants MIT/BSD/zlib/Apache-2.0 restent compatibles avec la GPL-3.0 ; FFmpeg et libmspack LGPL aussi.
   Il faut ajouter `COPYING` (GPL-3.0), `THIRD_PARTY.md` (reprendre les avis de Skate et Trials), et garder les en-têtes d'origine.
2. `native_gpu/` actuel (original, MIT/BSD seulement) peut rester sous la licence que l'on choisit ; le rendre GPL-3.0 est le
   plus simple une fois fusionné avec le code de Skate.
3. Le code de **Trials** n'a pas de licence : sans accord, il est « tous droits réservés ». Kurtisone en est co-auteur, mais TekRantGaming
   (46 commits, TRG Launcher) doit **donner son accord** (idéalement par un `LICENSE` GPL-3.0 ajouté au dépôt Trials). Le code de
   snufflyyy est MIT : réutilisable avec son avis. Le TRG Launcher n'est pas repris (XenonDash a son propre lanceur P5).
4. Les fichiers Microsoft/EA/RedLynx ne sont jamais versionnés : rien à changer, c'est déjà la règle des trois projets. Les
   dossiers `generated/` (traduction du code du jeu) restent locaux, ne sont pas distribués, et la liste d'adresses des recettes
   ne contient que des adresses (comme `codegen.toml` de Trials).
5. Le fait que Skate soit sous GPL à cause d'UnleashedRecomp n'est pas une charge pour le Dashboard si l'on garde `native_gpu/` séparé ;
   mais « une seule bibliothèque de rendu » (§3) signifie GPL-3.0 pour tout XenonDash. À décider par l'utilisateur avant l'étape 3.

## 3. Le rendu natif de Skate en détail

### 3.1 Architecture

Skate lie statiquement le D3D du XDK 2.0.5632. Le principe : **ne pas émuler le processeur de commandes**, remplacer le D3D par
des fonctions hôte (comme UnleashedRecomp pour Sonic, qui a le même D3D).

| Fichier (`src/native_gpu/`) | Lignes | Rôle |
|---|---|---|
| `native_gpu.cpp` | 75 | Active via `native_gpu`, crée le périphérique plume (Vulkan, Android : `libadrenotools`), démarre registre, capture, rendu en direct |
| `live_renderer.cpp` | 762 | Reçoit les appels D3D (`LiveRendererOnCall`) : Begin/EndVertices, DrawIndexedVertices, DrawVertices, ClearF, BeginTiling, Resolve, Swap ; lit l'état dans l'ombre du périphérique ; fil de rendu avec file de 2 images, présentation, protection en écriture des pages |
| `renderer.cpp` + `renderer.h` | 1 777 + 359 | Moteur : états de pipeline depuis les registres Xenos, tas de descripteurs bindless, cibles et EDRAM (propriété des tuiles), résolutions, textures détuilées, tampons de sommets/indices, cache de pipelines, échelle de rendu, gamma, format d'image |
| `shader_registry.cpp` | 154 | Retrouve le SPIR-V d'un shader par **hachage XXH3 du microcode** (clé choisie car D3D modifie les conteneurs en place) ; analyse les `vfetch` du shader de sommets pour construire la déclaration d'entrée (Skate n'a pas de déclarations de sommets) |
| `frame_capture.cpp` | 262 | Capture d'images (`.sk1c`, `native_gpu_capture_seconds`) : enregistre les événements et l'état ; point d'entrée du crochet |
| `xenos_decode.h`, `shader_cache.h`, `shaders/` | 489, 30 | Décodage de formats Xenos ; cache généré ; shaders HLSL de la couche (détuilage, résolution) compilés par `compile_native_shaders.sh` |
| `unleashed/` | 7 882 + 413 | Copie de référence de `video.cpp` (non compilée) |
| `tools/native_replay/replay.cpp` | — | Rejoue une capture hors ligne, produit un PPM (`REPLAY_REPEAT/SKIP/DUMP`) |
| `tools/d3d/capture.py` | — | Lit une capture : événements, passes, cibles |

**Dépendances au SDK** : sans le patch `native_guest_renderer.h`/`native_rhi.h` (inclus dans le patch 0001), le SDK ne sait pas laisser un
rendu natif présenter son image (`SetNativeGuestOutputRenderer`), ni ignorer les tirages invités (`gpu_skip_guest_draws`, laisse
le fil du processeur de commandes aux seules barrières/interruptions/échanges). XenonDash, lui, n'a eu besoin d'aucune modification
du SDK (GPU nul + fenêtre SDL + Vulkan direct) : deux philosophies à réconcilier.

### 3.2 Dépendance au jeu

- **Adresses codées en dur** : peu, mais cruciales. 14 crochets de fonctions, 34 occurrences de `0x82…` dans `src/`. Le sélecteur de
  `LiveRendererOnCall` traite **sept** appels : `BeginVertices 82CB29D8`, `EndVertices 82CB2E78`, `DrawIndexedVertices 82CB38E8`,
  `DrawVertices 82CB3500`, `ClearF 82CC7510`, `BeginTiling 82CC4FF0`, `Resolve 82CC7FF0` (+ Swap `82CCBE98`) ; plus deux constantes de
  projection pour le format d'image (`0x82191174`, `0x82191258`) et l'adresse du périphérique (`0x40002200`).
  `docs/d3d_functions.md` liste ~95 points d'entrée D3D (SetTexture `82CCB4D0` ~1 500/image, SetStreamSource `82CB9400`, SetIndices
  `82CB95A8`, ~60 setters d'état `82B12CB0..82B131A4`) avec un indice de confiance « sûr / probable / ? ».
- **Disposition de l'ombre du périphérique** (propre à la version du XDK) : bits sales à +0..+40, fetch constants, constantes de sommets à
  +1920 et de pixels à +6016, registres d'état à +10112..+10832, tampons à +12428/+12452/+12688. Cette disposition est quasi
  identique d'un jeu à l'autre **pour une même version de XDK** (le portage d'UnleashedRecomp repose sur ce fait) mais change
  d'une version à l'autre : le D3D du Dashboard 6770 est un autre XDK (adresses `0x9212xxxx`, API par dessin direct `DrawVerticesUP`,
  où l'état est suivi par appel de fonction, pas par ombre).
- **Aucun motif de code** : les adresses ont été trouvées à la main (trace d'appels + désassemblage). Il n'existe pas encore de
  dérivation automatique par motif. Pour un autre jeu, tout est à refaire, sauf la couche Vulkan.
- **Spécifique à Skate** : pas de `CreateTexture`/`Lock` (EA remplit ses en-têtes lui-même : suivi par adresse invitée avec détection
  d'écriture), tampons de sommets en écriture tardive (les appels sont rejoués au `Swap`), 4x MSAA en 3 tuiles, LUT 3D finale,
  compression RefPack des shaders dans les archives `.big`.

### 3.3 Cache de shaders

- XenosRecomp vendorisé (`third_party/XenosRecomp`, sur `990d03b`, MIT) + bibliothèques (fmt, xxHash, zstd, smol-v, `dxc-bin` réduit au
  compilateur Linux x64). Modifications : clés de cache par microcode (`XENOS_RECOMP_MICROCODE_KEY=1`), SPIR-V non compressé,
  normales demi-flottantes (`SPEC_CONSTANT_HALF_NORMAL`).
- `tools/d3d/build_shader_cache.sh` (s'exécute dans le conteneur, une fois le SDK construit) : (1) compile XenosRecomp ; (2)
  `harvest_shaders.py` extrait les shaders des archives du jeu (~3 min ; ceux de `render.big`/`marquee.big` en clair, les autres dans des
  flux RefPack) ; (3) `rexglue dump-xex` décode l'image et `image_shaders.py` en tire les 26 shaders compilés dans l'exécutable ;
  (4) XenosRecomp produit `out/native/shader_cache.cpp` (**348 shaders** pour Skate en jeu libre) compilé dans le binaire.
- Aucun fichier du jeu n'entre dans le dépôt ; le cache est local et dérivé. Le Dashboard 6770 fait la même chose
  (`work/shaders_out`, noms `ui_<adresse>_VS|PS.spv`) mais lit des fichiers à l'exécution au lieu d'un `.cpp` compilé ;
  les deux manières doivent être unifiées (un fichier de cache binaire par recette, plus simple à produire à la volée sans recompiler).
- Trois variantes de correctifs XenosRecomp à fusionner (§4.4).

### 3.4 Performances et état

Skate : 60 FPS sur Steam Deck (médiane 59,9 ; fil de rendu 4,6 ms) et 59 FPS sur Odin 2 en 1080p ; banc PC 37 FPS natif contre 26 émulé
(sans plafond). Reste ouvert : accrocs sur textures neuves, une image de feuillage vert vue une fois.

### 3.5 Comparaison avec `native_gpu/` de XenonDash

| | Skate1Native | XenonDash `native_gpu/` |
|---|---|---|
| Origine | Port de l'architecture d'UnleashedRecomp (GPL-3.0) sur plume | Écrit à part, Vulkan direct, sans code d'Unleashed |
| API Vulkan | plume (RHI, bindless, héritage Unleashed) | Vulkan 1.3 brut, SDL sans greffon |
| Point d'accrochage | Crochet après appel (`LiveRendererOnCall`) + ombre du périphérique ; 8 fonctions | Enveloppeurs forts de 3 281 fonctions ; état suivi par appel (`DrawVerticesUP`, `SetTexture`…) |
| Modifs SDK | Oui (présentation native, saut des tirages) | Aucune (GPU nul + vblank) |
| Cibles/EDRAM/résolutions | Modèle complet (tuiles, propriété, résolutions, MSAA, échelle) | Cibles hors écran, scissor, profondeur, échantillonneurs : **reste à faire** (P7 §11.4) |
| Textures | Détuilage en shader de calcul + cache par page | Détuilage CPU 2D, DXT1/3/5, formats usuels |
| Shaders | Cache XXH3-microcode compilé dans le binaire | SPIR-V lus au disque, clé = adresse du conteneur |
| Capture/rejeu | Oui (`.sk1c`, `native_replay`) | Trace `d3d_trace`, comparaison à l'oracle `xenos` (`framecmp.py`) |
| Maturité | 60 FPS en jeu | Écran Games du Dashboard reconnaissable, partiel |

### 3.6 Extraire en bibliothèque commune : ce qu'il faut

Découpage proposé de `xenondash_ngpu` (OBJECT/statique CMake, comme `xenondash_common`) :

1. **Noyau** (`ngpu_core`, indépendant du jeu et du XDK) : périphérique Vulkan, présentation SDL, file de rendu, tas bindless, cache de
   pipelines, cibles + modèle EDRAM + résolutions, textures (détuilage, formats, cache par page avec surveillance d'écriture), tampons,
   registre de shaders par microcode, cache de shaders binaire, gamma/aspect/échelle, capture/rejeu. C'est l'essentiel de
   `renderer.cpp`, `xenos_decode.h`, `shader_registry.cpp`, `frame_capture.cpp`, `native_replay`.
2. **Façades D3D** (`ngpu_front_*`), une par version de XDK, produisant le même flux de « DrawCall + état » : (a) *façade à ombre de
   périphérique* (Skate, XDK 5632, reprise de `live_renderer.cpp`) ; (b) *façade à fonctions D3D* (Dashboard 6770, reprise de
   `hooks_draw.cpp`/`d3d_trace.cpp`). Chacune dépend d'une **table d'adresses** par XEX (profil), jamais de constantes dans le code.
3. **Profil de jeu** (dans la recette, §5) : adresses D3D, adresse du périphérique, constantes de projection, tables de shaders.
4. **Dérivation des adresses par motif** : une signature par fonction D3D (suite d'instructions normalisée, ex. écriture du registre
   `RB_COPY_*` pour Resolve ; lecture de `+356` pour fibres), recherchée dans l'image décodée (`dump-xex`) ; étape déjà prévue au P7 pour
   13604/17559 et à réutiliser pour Skate afin de valider la recette sur plusieurs versions (Title Updates).
5. **Hors noyau** : menus RB+Start, page vidéo des options, saut d'intro, caméra large (par jeu).

**Recommandation** : (a) Partir du code de Skate pour le noyau (il est le plus mûr et il vise 60 FPS sur Deck), donc XenonDash en GPL-3.0 ;
(b) transférer le Dashboard 6770 sur ce noyau en gardant `native_gpu/` actuel comme second oracle jusqu'à la parité (jalon P7), puis
l'écarter ; (c) ne pas fusionner le rendu par flux de commandes de Trials : c'est une troisième voie (5 266 lignes, patch SDK 0008
« native command sink ») utile seulement pour les titres dont l'état n'existe que dans le flux de commandes (mémoire-export,
textures virtuelles de Fusion). À conserver comme **module optionnel** `ngpu_front_pm4` tant que Fusion n'a pas la parité.
(d) Garder l'émulation GPU du SDK **disponible** (cvar) : Trials en dépend aujourd'hui par défaut, et un jeu sans recette ni profil
n'a pas d'autre moyen de s'afficher. La contrainte du PLAN « sans repli sur l'émulation » (P8) doit être nuancée : « sans repli
pour les jeux ayant un profil natif ; avec repli explicite sinon ».

## 4. Les correctifs SDK : recouvrements et SDK unifié

### 4.1 Ce que chaque fork contient

| Fork | Base | Contenu | Rôle |
|---|---|---|---|
| XenonDash | `c94f5eb` | 6 commits : `ff47726` table des fonctions dans le tas de l'image ; `5d18a61` touches synthétisées (clavier/souris) ; `c66e8ae` arguments u64 des fonctions noyau/XAM ; `6ea65b1` imports sans thunk (codegen) ; `df2bd73` chargement de `libXXX.so` voisins ; `4af94a8` DPC + HMAC-SHA1 | Runtime/noyau/codegen **génériques** (aucun GPU) |
| Skate (vendorisé) | `c94f5eb` + Skate 3 + propre | ~270 fichiers : `cmake` (export de cibles, version), `include/rex/ui` (22 : overlays, polices, listes de GPU), `include/rex/graphics` (18 : `native_guest_renderer.h`, `native_rhi*.h`, télémétrie d'horloge, profil NVIDIA), `include/rex/input` (9), audio SDL/XMA, `src/ui/vulkan` (6), `src/graphics/vulkan` (6), `src/kernel/xam`, `std_compat.h`, `main_android.h`, templates de codegen, tests PPC | Mélange : Android/ARM64, Vulkan, UI de réglages, **hook de rendu natif**, correctifs d'émulation GPU de Skate (profondeur float24, effacement de pages…) |
| Trials `linux-android/0001` | v0.10.0 | Le travail de Skate (298 fichiers) + correctifs de rendu de Trials | = Skate + Trials |
| Trials `linux-android/0002` | idem | Correctifs de rendu D3D12 reportés sur le traducteur SPIR-V (récupération d'entiers, LOD, résolutions 16/32 bpp, export mémoire) | Émulation GPU Vulkan |
| Trials `0003`–`0007` | idem | nom du dossier d'application Android ; soulignés conservés dans `rexglue.cmake` ; jointure unique d'un fil terminé (bionic abandonne sinon) ; envois rapides sur Qualcomm ; contrôles flottants Vulkan désactivables | Runtime générique (dont `0004`, `0005` utiles à Linux) / Android |
| Trials `0008` | idem | *Native command sink* : le processeur de commandes remet chaque tirage/copie/échange avec le fichier de registres et les shaders liés | Rendu natif par flux de commandes (Fusion) |
| Trials `0001`–`0021` | idem | Correctifs D3D12 (entiers, LOD, export mémoire, résolutions, mémoire partagée, présentateur FSR/NIS, journal de paquets) ; `0018`, `0021` pour Vulkan | Émulation GPU ; **Windows/D3D12 en majorité** ; déjà fusionnés dans `linux-android/0001/0002` pour Linux |

### 4.2 Recouvrements et conflits (mesurés)

- **XenonDash ∩ Skate** : 7 fichiers modifiés par les deux (`mnk_input_driver.{h,cpp}`, `kernel_state.{h,cpp}`, `xmemory.cpp`,
  `phase_register.cpp`, `xboxkrnl_threading.cpp`). `git apply --check` de `linux-android/0001` sur notre SDK : **échec seulement** sur
  `mnk_input_driver.{h,cpp}` ; le reste s'applique, et `0002..0008` s'appliquent ensuite. Skate contient déjà `KeInsertQueueDpc`,
  `XeCryptHmacSha*`, `function_table`, `setjmp`, fibres (même base) : à comparer ligne à ligne pour voir si notre `4af94a8` fait doublon ou
  corrige un défaut.
- **Skate ↔ Trials** : pas de conflit (Trials a absorbé Skate). Divergence future : Skate continue de modifier son SDK vendorisé.
- **Trials 0001–0021 ↔ Skate/Trials linux-android** : 0001–0021 ne s'appliquent plus après `linux-android/0001` (déjà fusionnés : vérifié,
  21 échecs « déjà appliqué »). Pas à rejouer pour Linux.
- **Doublons de rôle** : le « clavier synthétisé » de XenonDash et les entrées de Skate ; la prise en charge de fibres (`rexcrt`) du SDK,
  utilisée par Trials, présente dans les deux bases ; l'export de `rexglue.cmake` (Trials 0004) et nos patchs de codegen.
- **XenosRecomp** : trois variantes incompatibles à fusionner. Skate : clés de microcode, SPIR-V brut, normales demi-flottantes (sur
  `990d03b`). Trials : `tools/native/xenosrecomp.patch` (473 lignes : export mémoire `eA/eM0..4`, `getCompTexLOD`, branche `else` sur la
  pile de prédicats, `cube`, coordonnées fenêtre, pixels invités, compteurs de boucles). XenonDash : `xenosrecomp-dash360.patch` (65 lignes :
  shaders sans table de constantes, registres/échantillonneurs non nommés, assertion retirée).

### 4.3 Proposition : un SDK unifié

Un seul dépôt `rexglue-sdk` (fork Forgejo, branche `xenondash`) en **série de correctifs ordonnée**, chaque correctif avec son rôle et son
interrupteur (cvar ou option CMake), appliquée au-dessus de `c94f5eb` :

1. `generic-runtime-*` (les 6 commits XenonDash, tels quels) ;
2. `linux-android-0001` en deux morceaux : (a) *plateforme* (cmake, export de cibles, audio SDL, entrées, UI, Android/ARM64, `std_compat`) ;
   (b) *rendu natif* (`native_guest_renderer.h`, `native_rhi*`, `gpu_skip_guest_draws`) ; conflit `mnk_input_driver` résolu en gardant la
   synthèse de touches XenonDash et en reprenant le reste de Skate ;
3. `linux-android-0002` (traducteur SPIR-V : correctifs de rendu Trials) ;
4. `0003..0007` (utilitaires : `0004`, `0005`, `0007` génériques ; `0003`, `0006` Android seulement, derrière CMake) ;
5. `0008` command sink (optionnel, option CMake `REX_NATIVE_COMMAND_SINK`) ;
6. fonctions XAM/noyau propres au Dashboard (déjà au point 1 ou dans `common/`).

**Ce qui devient inutile sans émulation GPU** : l'ensemble D3D12 (0001–0021 sauf à garder `0018/0021` côté Vulkan), le présentateur FSR/NIS
du SDK (remplacé par la présentation du noyau), les options `readback_*`, `depth_float24_*`, `clear_memory_page_state` de Skate. **Ce qui reste utile
tant que Trials utilise l'émulation par défaut et que le Dashboard garde `xenos` comme oracle (P7)** : le traducteur SPIR-V et les
correctifs de résolution. Il faut donc *conserver* ces correctifs, mais les isoler pour pouvoir compiler un SDK « sans GPU invité »
(`REX_GPU_EMULATION=OFF`) plus léger et plus rapide à compiler. Les correctifs `PM4`/D3D12 pur Windows peuvent rester hors du fork (inutiles sous Linux).

**Condition d'acceptation** : après chaque ajout, build du fork, Dashboard 6770 (émulation puis `-DDASH_NATIVE_GPU=ON`), Skate (natif),
Fusion (émulé) ; test `tools/test_appimage.sh` inchangé.

### 4.4 XenosRecomp unifié

Un seul fork vendorisé (ou correctifs `patches/xenosrecomp-*.patch` ordonnés) : (1) tables de constantes absentes (Dashboard) ;
(2) clés par microcode + SPIR-V brut + normales demi-flottantes (Skate) ; (3) export mémoire, LOD, `else`, `cube`, pixels invités (Trials).
Chacun derrière une variable d'environnement ou une option de la ligne de commande, jamais comportement implicite : les clés de cache
changent si les sorties changent, donc le cache doit inclure la version de XenosRecomp et des correctifs.

## 5. Format de recette et flux de recompilation à la demande

### 5.1 Recette `games/<TitleID>/`

Le dépôt ne contient que des **adresses, des paramètres et du code de port**, jamais d'octet du jeu. Par exemple :

```
games/<TitleID>/                 # <TitleID> en hexadécimal majuscule (ex. 58411401)
  recipe.toml                    # méta + versions acceptées + options de build
  codegen.toml                   # setjmp/longjmp, [rexcrt], noms (adresses seulement)
  functions.toml                 # bornes de fonctions ajoutées à l'analyse
  missing_funcs.toml             # (si besoin) fonctions manquantes détectées
  runtime.toml                   # valeurs par défaut du runtime (touches, GPU, quit_dialog)
  d3d_profile.toml               # profil de rendu natif : façade, adresses D3D, adresse du périphérique, constantes
  patches/                       # correctifs SDK/XenosRecomp propres au jeu (rares)
  src/                           # hooks C++ du port, compilés en module recompilé, pas de code du jeu
  extract/                       # description des extractions (ISO/STFS/GOD → game_root)
  tests/                         # attentes de vérification (hachages d'images décodées, captures de référence)
```

`recipe.toml` (schéma proposé) :

```toml
schema = 1
title_id = "58411401"
name = "Trials Fusion"
sdk = { series = "xenondash-1", min_revision = "0.10.0+xd1" }     # SDK unifié (§4)
xenosrecomp = "xd1"
# Exécutables reconnus : hachage du XEX *décrypté/décompressé* (image PE), pas du fichier d'origine
[[xex]]
version = "0.0.0.3"                  # lu dans l'en-tête, vérifié en plus du hachage
sha256_image = "<à calculer par dump-xex lors de la mise au point>"
media_id = ""                        # facultatif
codegen = "codegen.toml"
[[tu]]                               # mises à jour de titre : variantes reconnues, chacune avec ses adresses
version = "0.0.0.4"
sha256_image = "..."
overrides = "tu4/"                   # variantes de codegen/d3d_profile
[build]
container = "ubuntu22-clang20"       # image documentée (§5.3) ; "host" accepté si clang ≥ 20
min_ram_gb = 16
min_disk_gb = 40
estimate_minutes = { codegen = 9, compile = 25, shaders = 5 }
[render]
mode = "native"                      # native | emulated
profile = "d3d_profile.toml"
[extract]
sources = ["iso", "stfs", "god", "folder"]
needs = ["default.xex", "data/"]     # ce qu'on lit dans la source
[features]                           # générique, activé par la recette
fps_modes = [30, 40, 60]
wide_aspect = true
```

Chaque `[[xex]]` est un **verrou** : hors des hachages listés, le lancement se fait en « mode générique » (§5.2, étape 6) avec avertissement.
Hachage de l'image décodée (plutôt que du `default.xex` chiffré) : indépendant de la compression/chiffrement de la source et de sa mise en paquet.

### 5.2 Flux

```
XenonDash voit le XEX (disque, Mes jeux, Games Library)
 1. identifie : lecture de l'en-tête (TitleID, version, MediaID) par xexinfo ; hachage de l'image décodée
 2. trouve une recette games/<TitleID>/ dont un [[xex]]/[[tu]] correspond
        non  -> jeu sans recette : proposition de tentative générique (boucle auto du P2), jamais d'exécution implicite
 3. cache : ~/.local/share/xenondash/games/<TitleID>/<hash>/ contient-il build.ok pour (hash XEX, rev. recette, rev. SDK, rev. XenosRecomp) ?
        oui -> aller à 7
 4. prépare : espace disque/RAM suffisants ? conteneur (podman/docker) ou clang hôte ? SDK pré-construit ? sinon message précis et sortie
 5. recompile (processus fils, écran de progression, annulable, reprise) :
        a. extraction des fichiers nécessaires dans le cache (jamais dans le dépôt)
        b. codegen   (rexglue codegen avec la recette)           ~ 9–15 min pour un gros jeu
        c. shaders   (harvest + XenosRecomp → shader_cache.bin)  ~ 3–5 min
        d. compilation (cmake + ninja, 12 cœurs)                 ~ 25 min
        e. vérification : test d'amorçage (sans affichage) → build.ok + journal
 6. échec -> journal conservé, bouton « signaler » (sans fichiers du jeu), cache partiel purgeable
 7. lance via le lanceur P5 (processus séparé, retour au Dashboard)
```

Écran de progression : étapes a–e avec durée estimée tirée de `recipe.toml` (corrigée par les mesures du dernier build), journal
repliable, annulation. Le cache est indexé par `(hash XEX, version de la recette, version du SDK, version de XenosRecomp)` ;
le changement d'un seul élément invalide ce qu'il faut (le cache de shaders se recalcule sans recoder).
**Nettoyage** : « Supprimer le cache du jeu », limite d'espace globale, purge des anciens `<hash>`.

### 5.3 Dépendances chez l'utilisateur

Skate et Trials utilisent le même conteneur (Ubuntu 22.04 : glibc 2.35 pour SteamOS ; clang 20, cmake Kitware, Vulkan SDK LunarG, en-têtes X11/Wayland/audio).
Il sera construit **une fois** (image publiée dans un registre, ou `Containerfile` dans le dépôt) et réutilisé par toutes les recettes. Sans
conteneur, `clang` hôte ≥ 20 convient (la machine de développement actuelle compile avec). Attendu par jeu : 40 Go de disque, 16 Go de RAM (les
fichiers de code recompilé sont énormes), 30 à 45 minutes sur un processeur à 12 cœurs. Aucun binaire de jeu n'est distribué : le cache est
privé à la machine et non redistribuable (cf. risque connu du PLAN).

## 6. Plan d'étapes chiffré (jours-personne, ± 30 %)

| # | Étape | Contenu | j |
|---|---|---|---|
| 0 | Décisions préalables | licence de XenonDash (GPL-3.0 ?), accord de TekRantGaming, périmètre « émulation en repli » (§3.6) | 1 |
| 1 | Inventaire | ce document | fait |
| 2 | SDK unifié | série de correctifs (§4.3), résolution de `mnk_input_driver`, doublons DPC/HMAC, options CMake, build + non-régression (Dashboard émulé et natif, Skate, Fusion) | 5 |
| 2b | XenosRecomp unifié | fusion des 3 variantes, options, tests de sortie sur les shaders des 3 projets | 3 |
| 3 | Licence et dépôt | `COPYING`, `THIRD_PARTY.md`, en-têtes, politique de `generated/` | 1 |
| 4 | Noyau de rendu commun | extraction de `renderer`/`registry`/`capture`/`replay` de Skate en `ngpu_core`, façade Skate sur le noyau (parité pixel avec les captures `.sk1c`), conversion du cache en fichier binaire | 12 |
| 5 | Dashboard 6770 sur le noyau | façade « fonctions D3D » (reprise de `native_gpu/`), parité avec l'oracle `xenos` ; retire l'ancien `vk_backend` ; coïncide avec le reste de P7 (≈ 24 j, dont ~10 gagnés par le noyau déjà complet) | 14 |
| 6 | Format de recette et moteur | `recipe.toml`, lecteur, hachage d'image (xexinfo), cache, processus de build, écran de progression, intégration du lanceur P5 | 9 |
| 7 | Skate en recette | `codegen.toml`/`functions.toml`, hooks C++ (pause, quit, caméra, intro), profil D3D, extraction ISO, cache de shaders ; compile et lance depuis le Dashboard | 6 |
| 8 | Trials (3 jeux) | recettes HD/Evolution/Fusion, hooks partagés, extraction STFS (`stfs_package` MIT de snufflyyy), émulation par défaut, module `pm4` optionnel pour Fusion ; construire HD et Evolution (jamais construits) | 11 |
| 9 | Recompilation générique | boucle auto P2 + recette minimale générée + dérivation D3D par motif pour un nouveau XEX | 10 |
| 10 | Stabilisation | AppImage avec le moteur de recettes, tests sur machine vierge, documentation | 4 |
| | **Total** | | **≈ 76 j** avec l'étape 5 entière ; **≈ 62 j** si l'on compte P7 déjà prévu (≈ 14 j) |

Jalons : « SDK unifié construit » vers 10 j ; « Skate rendu par le noyau commun » vers 22 j ; « Skate lancé depuis le Dashboard » vers 37 j
(étape 5 menée en parallèle) ; « Trials » vers 48 j.

## 7. Risques

1. **Licence** : XenonDash GPL-3.0 obligatoire dès la reprise du code de Skate ; l'accord de TekRantGaming pour le code de Trials n'est pas acquis.
   Atténuation : décider tôt (étape 0), ne reprendre de Trials que le MIT et les parties dont Kurtisone est seul auteur.
2. **Dérive des SDK** : Skate et Trials continuent d'évoluer pendant la fusion. Atténuation : geler un commit de chaque projet (§1) ; la
   série de correctifs référence des hachages.
3. **Rendu commun trop ambitieux** : la façade à ombre de périphérique de Skate dépend d'un XDK précis ; un autre jeu (ou le Dashboard) exige
   de refaire la cartographie à la main (Dashboard : 3 281 fonctions, Skate : ~95). La dérivation par motif n'existe pas encore. Atténuation :
   noyau + une façade par XDK, profil dans la recette, repli émulation ; ne promettre l'automatique que pour les XDK déjà cartographiés.
4. **Parité d'image** : le Dashboard natif est partiel (cibles hors écran, scissor, profondeur). Le noyau de Skate le règle probablement, à
   vérifier ; sinon l'oracle `xenos` doit rester jusqu'à la parité.
5. **Temps, disque, mémoire** : 30 à 45 min et 40 Go par jeu, 16 Go de RAM (swap ou échec sinon). Atténuation : vérification préalable
   (Skate le fait déjà), niveau de parallélisme réglable, reprise.
6. **Chaîne de compilation chez l'utilisateur** : podman/docker, clang 20, noyau récent ; images conteneur lourdes ; réseau pour le
   premier téléchargement. Atténuation : image documentée, mode hôte, message d'erreur précis.
7. **Versions et mises à jour de titre** : un jeu mis à jour (autre `default.xex`) casse les adresses. Les hachages verrouillent ; il faut un
   mode « version inconnue » explicite plutôt qu'une exécution hasardeuse.
8. **Reproductibilité du cache** : toute modification de SDK/XenosRecomp change les shaders et les clés ; cache indexé par version (§5.2).
9. **Distribution** : le binaire recompilé est une traduction du code du jeu : pas de redistribution, AppImage seulement pour usage personnel (déjà au PLAN).
10. **Trials HD et Evolution jamais construits sous Linux** ; Fusion vert (bug connu de l'image) et Android seulement validé sur Turnip.
11. **Régressions du Dashboard** lors du changement de SDK (le Dashboard est le seul « jeu » validé de bout en bout aujourd'hui). Atténuation :
    garder `tools/test_appimage.sh` et les tests d'image 6770/6717/13604 comme barrière à chaque étape.

## Annexe : sources lues (lecture seule)

`Skate1Recompiled` : `README.md`, `THIRD_PARTY.md`, `COPYING`, `NOTES.md`, `docs/{native_d3d,d3d_functions,building}.md`, `build-linux.sh`,
`skate1_manifest.toml`, `config/*`, `src/*`, `src/native_gpu/*`, `tools/d3d/*`, `tools/steamdeck/Containerfile`, SDK vendorisé comparé à `c94f5eb`.
`TrialsReVolution` : `README.md`, `BUILDING.md`, `THIRD_PARTY_NOTICES.md`, `docs/{native_port,linux_android}.md`, `build-linux.sh`, `tools/setup_sdk.sh`,
`sdk-patches/**`, `games/**` (hors `generated/`), `src/*`, `tools/native/xenosrecomp.patch`, `tools/linux/Containerfile`.
XenonDash : `PLAN.md` (P7/P8), `README.md`, `docs/rendu_natif.md`, `native_gpu/`, `patches/`, historique de `third_party/rexglue-sdk`.

## 8. Avancement : SDK unifié et XenosRecomp unifié (P8, étapes 2 et 2b, 2026-11-12)

### 8.1 SDK unifié : branche `unified` du fork (`third_party/rexglue-sdk`)

Branche `unified` partant de `xenondash` (`4af94a8`, non réécrite), poussée sur le remote `xenondash`. 11 commits, un par correctif
d'origine (auteur d'origine conservé : tous de Kurtisone) :

| Commit | Source | Contenu |
|---|---|---|
| `e28335b` | Trials `linux-android/0001` (= travail de Skate1Native), partie plateforme | cmake, audio SDL/XMA, entrées, UI, noyau/XAM, codegen, système, tests PPC |
| `688fc58` | idem, partie crochet de rendu natif | `native_guest_renderer`, `native_rhi{,_vulkan}`, `gpu_skip_guest_draws`, télémétrie d'horloge, profil NVIDIA |
| `e45dbc2` | idem, partie graphique restante | processeur de commandes, caches, traducteurs de shaders, Vulkan (émulation, **conservée**, cf. §4.3) |
| `95f3ab9` | Trials `0002` | correctifs de rendu Trials du traducteur SPIR-V/Vulkan |
| `a1f7b2b` | Trials `0003` | nom du dossier d'application Android (option de build) |
| `00d4775` | Trials `0004` | `rexglue.cmake` garde les soulignés du nom de projet |
| `de4cb85` | Trials `0005` | jointure unique d'un fil terminé |
| `b9e3d23` | Trials `0006` | envois Vulkan rapides sur Qualcomm |
| `e2bb408` | Trials `0007` | contrôles flottants Vulkan désactivables |
| `b973c1f` | Trials `0008` | *native command sink* (inactif tant qu'aucun jeu n'enregistre de crochet) ; patch d'origine sans en-tête d'auteur |
| `091e6d3` | XenonDash (compatibilité) | alias `PPCImageConfig`/`PPCFuncMappings` dans le pch généré, voir ci-dessous |

Les six commits XenonDash (`ff47726`..`4af94a8`) sont le socle : fonction table dans le tas, DPC, HMAC-SHA1, arguments u64, imports sans
thunk, `libXXX.so`. Ils ne faisaient pas doublon : le SDK de Skate/Trials ne contient ni `KeInsertQueueDpc` fonctionnel ni la lecture u64
(`4af94a8` est conservé tel quel). Le SDK vendorisé de Skate (comparé fichier par fichier) n'apporte **rien** de plus que
`linux-android/0001..0008` : Trials en est un sur-ensemble.

**Écartés (non appliqués)**
- Tous les fichiers D3D12 de `0001` (`src/graphics/d3d12/*`, `include/rex/graphics/d3d12/*`, `src/ui/d3d12/*`, bytecodes D3D12 5.1,
  `native_rhi_d3d12`) : émulation GPU D3D12 de Windows, inutile sous Linux/Vulkan (le build Linux affiche `D3D12=OFF`). Windows/D3D12 n'est
  donc pas garanti sur `unified`.
- `sdk-patches/0001..0021` de Trials (D3D12, mémoire partagée, présentateur FSR/NIS, journal de paquets) : déjà fusionnés dans
  `linux-android/0001/0002` pour Linux, ou D3D12 pur.
- Non fait (reste) : l'isolation `REX_GPU_EMULATION=OFF` (§4.3) ; les options d'émulation de Skate (`depth_float24_*`, `readback_*`,
  `clear_memory_page_state`) sont encore compilées, désactivées par défaut.

**Conflits résolus**
1. `mnk_input_driver.{h,cpp}` (XenonDash `5d18a61` « touches synthétisées » contre Skate) : les deux comportements sont gardés. La file de
   touches liées de Skate (`RefreshBoundKeystrokesLocked`) est servie d'abord ; la synthèse à partir de l'état manette émulé reste le repli
   (le Dashboard ne lit que `XamInputGetKeystrokeEx`) ; la mémoire de la synthèse est alignée sur chaque événement servi de la file, pour
   ne pas doubler les appuis, la répétition automatique est conservée ; `key_latched_` et `RefreshBoundKeystrokesLocked` sont remis à zéro
   à la perte du focus. Les autres fichiers communs (`kernel_state`, `xmemory`, `phase_register`, `xboxkrnl_threading`) fusionnent seuls.
2. **Rupture d'API de génération** trouvée à la validation : `0001` préfixe les symboles générés par le projet (`dash_PPCImageConfig`) ; les
   `app.h` des Dashboards utilisent `PPCImageConfig`. Le commit `091e6d3` ajoute des alias dans le pch généré (projets à une seule image
   inchangés ; les projets multi-images utilisent les noms préfixés).

**Validation** (build du SDK dans le préfixe séparé `sdk-unified/`, puis `tools/port_dashboard.py --sdk-prefix sdk-unified --stages
codegen,build,smoke` sur une copie des projets, `work/unified-test/`, pour ne pas toucher aux projets de l'autre agent) :

| Dashboard | codegen + build | smoke (Xvfb + lavapipe, 40 s, aucun FATAL) |
|---|---|---|
| 2.0.6770.0 | OK | OK |
| 2.0.6717.0 | OK | OK |
| 2.0.13604.0 | OK | OK |
| 2.0.17559.0 | OK | OK |

Non rejoués : `tools/test_appimage.sh`, `DASH_NATIVE_GPU=ON`, Skate et Fusion (non disponibles ici), test clavier interactif de la fusion de
`mnk_input_driver` (le smoke ne l'exerce pas). Piège rencontré : l'étape de build de CMake relance `rexglue codegen` quand le SDK change ;
lancer la validation directement dans `dashboards/<version>` réécrit leurs `generated/` (régénérés ensuite avec `sdk/`, sans perte) :
d'où `--dest` sur une copie.

**Bascule** : le sous-module `third_party/rexglue-sdk` du superprojet pointe sur `unified` ; `sdk/` est réinstallé depuis `unified`
(`sdk/bin/rexglue --version` : `0.10.0.19-dev.g091e6d3`). L'ancien `sdk/` est sauvegardé dans `work/sdk-xenondash-backup/` (non versionné).
Les projets de Dashboard déjà construits régénèrent leur code au prochain build (le binaire `rexglue` a changé).
`patches/rexglue-sdk-000{1,2,3}-*.patch` restent valables comme historique mais ne sont plus à appliquer.

### 8.2 XenosRecomp unifié : `patches/xenosrecomp/` + `tools/build_xenosrecomp.sh`

Aucun dépôt créé : une série de 4 correctifs sur l'amont `hedge-dev/XenosRecomp` (`990d03b`), appliquée par `tools/build_xenosrecomp.sh`
dans `work/XenosRecomp-unified` (clone de l'amont, application des correctifs avec empreinte de la série, compilation ; ≈ 30 s).

| Correctif | Source | Comportement |
|---|---|---|
| `0001-skate1native-*` | copie vendorisée de Skate | `XENOS_RECOMP_MICROCODE_KEY=1` (clé = XXH3 du microcode), `XENOS_RECOMP_NO_ZSTD=1`, `SPEC_CONSTANT_HALF_NORMAL`, registres non nommés `xr_cN` |
| `0002-xenondash-*` | `patches/xenosrecomp-dash360.patch` rebasé | table de constantes absente, assertion retirée, échantillonneurs non nommés (le volet « registres non nommés » est remplacé par `xr_cN`, même effet) |
| `0003-trials-*` | `tools/native/xenosrecomp.patch` de Trials, tel quel | export mémoire, LOD, biais/exposant, `cube`, pixels invités, boucles |
| `0004-trials-extensions-*` | nouveau | extensions de Trials **seulement avec `XENOS_RECOMP_TRIALS=1`** (sortie `#define XENOS_TRIALS_EXT`) : elles lisent des constantes partagées (décalages 276..412) que `native_gpu/` ne fournit pas |

Vérification : `tools/xenos_batch.py work/shaders6770/ui <sortie> work/XenosRecomp-unified` : **70/70 shaders UI traduits** (52 PS + 18 VS) ;
les 70 SPIR-V sont **octet pour octet identiques** à ceux de l'ancien XenosRecomp patché (`work/shaders_out`). Avec
`XENOS_RECOMP_TRIALS=1` et avec `MICROCODE_KEY`+`NO_ZSTD`, 70/70 également. Les clés de cache doivent inclure l'empreinte de la série et ces
variables (§4.4). `patches/xenosrecomp-dash360.patch` est conservé comme historique, remplacé par la série.
Reste : tester la série sur des shaders de Skate et de Trials (non disponibles ici) ; brancher `tools/xenos_batch.py` par défaut sur
`work/XenosRecomp-unified`.
