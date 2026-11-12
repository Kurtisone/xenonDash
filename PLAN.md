# Plan : tous les Dashboards Xbox 360 en natif Linux, un AppImage par version

## Objectif

**Une version par interface** (décision du 2026-11-06) : la plus récente de chaque génération —
**Blades** (2.0.6770, fait), **NXE** (dernière version NXE avant Metro, à identifier) et **Metro** (2.0.17559) —
recompilée en natif, livrée sous forme d'**un AppImage par interface**, capable de **lancer les jeux**. Les fichiers Microsoft (firmware, XEX, polices)
ne sont jamais versionnés : l'AppImage les importe depuis une mise à jour officielle fournie par
l'utilisateur au premier lancement.

## Décisions

| Sujet | Décision | Raison |
|---|---|---|
| Runtime | **Garder ReXGlue, en fork** (`third_party/rexglue-sdk` → dépôt Forgejo `rexglue-sdk`, branche `xenondash`) | Il fournit noyau HLE + GPU Xenos/Vulkan + audio + entrées. Le remplacer = réécrire Xenia. Nos 3 correctifs sont des commits du fork (**fait** : sous-module git, branche `xenondash`). Réévaluer seulement si un blocage du SDK devient insoluble. |
| XAM | **Recompilé** (module DLL ReXGlue) quand un `xam.xex` complet et déchiffrable de la **même version** existe ; sinon **émulation haut niveau** (HLE ReXGlue + `dash_*.cpp`) | Mélanger Dashboard et XAM de versions différentes n'a jamais existé sur console. |
| Versions Blades (≤ 7258) | HLE pour l'instant (XAM 6770 = base 1888 indéchiffrable + delta) ; recompilation de XAM si une base déchiffrable est obtenue | — |
| Versions NXE/Metro avec XAM complet (17559, autres mises à jour complètes) | Dashboard **et** XAM recompilés | Écrans XAM d'origine (succès, Guide…) au lieu des remplaçants ImGui. |
| Versions à porter | **Une par interface** (Blades 6770, NXE la plus récente, Metro 17559) ; les autres versions (6717, 7258, 5759, 4532, 2255) restent en l'état, sans effort supplémentaire | Les versions d'une même interface diffèrent peu ; l'effort va aux trois interfaces. |
| Natif maximal, zéro émulation | Décision du 2026-11-06 : (1) **GPU** : remplacer l'émulation Xenos (processeur de commandes → Vulkan) par un **rendu natif au niveau D3D** : les fonctions D3D de la XDK liées statiquement dans dash.xex/xam.xex sont remplacées (REX_HOOK) par une implémentation Vulkan, shaders traduits hors ligne (XenosRecomp), comme UnleashedRecomp ; (2) **XAM** : recompilé partout où un xam.xex complet existe (NXE 13604, Metro 17559 ; Blades si NAND) ; (3) **noyau** : réimplémentation native (déjà le cas, ce n'est pas de l'émulation) ; (4) **jeux** : uniquement des ports natifs, Xenia retiré du plan (option désactivée par défaut). | Demande de l'utilisateur. L'espace d'adressage invité big-endian reste : il fait partie du code recompilé, pas d'une émulation. Étude de faisabilité du rendu D3D natif : [docs/rendu_natif.md](docs/rendu_natif.md) ; **premier jalon atteint sur 6770 (§11 du document : écran Games rendu nativement par `native_gpu/`, option `DASH_NATIVE_GPU`, ≈ 50 images/s contre ≈ 12 avec l'émulation sous lavapipe)**. |
| Licence et P8 (2026-11-08) | XenonDash sous **GPL-3.0** (reprise du code de Skate1Native) ; **zéro émulation stricte** : un jeu sans rendu natif n'est pas lançable ; l'utilisateur a l'**accord de TekRantGaming** pour reprendre le code de TrialsReVolution. | Décisions de l'utilisateur. |
| Lancement des jeux | `XamLoaderLaunchTitle` → lanceur : **port natif enregistré** (ex. Skate1Recompiled) pour ce TitleID, sinon **Xenia** (Linux) avec le XEX/ISO ; retour au Dashboard à la fin du jeu | Un jeu ne s'exécute nativement que s'il est lui-même recompilé. |
| Modèle d'exécution | Les tâches sont confiées à des agents **Sonnet** ; revue et intégration par la session principale | Demande de l'utilisateur (coût). |

## Phases

### P0 — Restructuration multi-versions (FAIT, sauf fork du SDK)
- `common/` : bibliothèque partagée par toutes les versions (profil, GPD/succès, entrées, audio
  silencieux, interfaces XAM de remplacement, montage flash, stubs).
- `dashboards/<version>/` : un projet ReXGlue par version (manifeste, `overrides.toml`, `app.h`).
  `dash/` actuel → `dashboards/2.0.6770.0/`.
- Fork du SDK sur Forgejo (correctifs 0001-0003 en commits), sous-module. **À faire.**
- Critère : 6770 se compile et se lance comme avant (`input_test.sh`).
- Fait : `common/` est une bibliothèque CMake **OBJECT** (`xenondash_common`) pour que les `REX_HOOK(__imp__...)`
  soient liés (vérifié avec `nm`) ; `xenondash::DashAppBase` porte la logique de `dash_app.h` (paramétrée par
  le nom du XEX) ; `sub_92739D9C` et `diag_hooks.cpp` restent dans `dashboards/2.0.6770.0/src/`.
  `dash/` → `dashboards/2.0.6770.0/` (`git mv`), scripts et `.gitignore` adaptés ; build et `input_test.sh`
  vérifiés (onglet Games, écran Achievements, aucun FATAL).

### P1 — Contenu : Stored Game Items et Downloads (HLE) (FAIT en grande partie)
- Contenu par profil/jeu dans `profiles/<XUID>/Content/<TitleID>/<type>/…` (sauvegardes 0x00000001,
  contenu téléchargé 0x00000002, démos, thèmes, images de joueur…), comme sur le disque dur.
- `XamContentCreateEnumerator` filtré par jeu, `XamContentGetDetails`/taille/miniature,
  suppression ; compteurs « N saves – N downloads » de Played Games.
- Outil d'import d'un paquet STFS (sauvegarde/DLC réels) : `tools/stfs.py` étendu (CON/LIVE/PIRS,
  métadonnées, miniature).
- Critère : Game Summary > Stored Game Items / Downloads listent le contenu importé.
- Fait (`common/src/dash_content.cpp`, `tools/content_import.py`, `tools/make_test_content.py`) : énumération agrégée
  (`XamContentAggregateCreateEnumerator(xuid, appareil, type, titre, &handle)`, élément 0x150 = données + taille BE64),
  énumérateurs simples, `GetMetaDataInternal` (en-tête STFS 0x344..0x971A), `GetLocalizedString`, `DeleteInternal` ;
  compteurs « 2 saves - 1 downloads » de Played Games corrects ; liste Games > Skate avec noms, types, miniatures,
  tailles réelles ; options Copy/Move/Delete et boîte de confirmation.
- Downloads : ouvre une invite « Sign in to Xbox LIVE to download content » (Marketplace en ligne, pas le contenu
  local) ; le contenu téléchargé local se voit dans Stored Game Items. Nécessiterait un état de connexion Live simulé.
- Fait ensuite : propriétaire correct (paquet de profil virtuel + `XamProfileLoadAccountInfo`, le Dashboard bâtit sa
  liste de profils avec les contenus de type 0x10000) ; liste Games regroupée par jeu (`XamUniSortCmpString` réel
  au lieu du stub) ; suppression vérifiée de bout en bout ; Copy/Move (`XamContentCopyInternal`/`MoveInternal`) :
  changement de propriétaire sur le disque, erreur propre vers un appareil non inscriptible.
- Stored Game Items non filtré : c'est le comportement du Dashboard 6770 (scène `DeviceSelector.xur` sans paramètre,
  voir README), pas un défaut de XAM.
- Reste : Downloads (Marketplace en ligne, demande une connexion Xbox LIVE simulée) ; noms d'appareils « Dummy HDD /
  Dummy ODD » (SDK) ; `XamContentGetAttributesInternal`, `ResolveInternal`, `GetDefaultDevice`, `OpenFileInternal`
  encore en stub (jamais appelés par le parcours Stored Game Items) ; Copy/Move entre profils non testés à l'écran.

### P2 — Chaîne de portage automatisée (FAIT)
- `tools/port_dashboard.sh <mise_à_jour|STFS|dash.xex> <version> [--flash-dir ...]` (wrapper de
  `tools/port_dashboard.py`) : extraction STFS, `assets/` (dash.xex, flash, `shrdres.xzp` converti), image PE
  (`xexinfo`), projet depuis le gabarit `tools/templates/dashboard/`, `rexglue codegen` + boucle de corrections
  (cibles non résolues, fonctions par pointeur / `lis`-`addi`, continuations de `catch` et tables SEH écartées par
  `filter_inside.py`) -> `overrides.toml` **généré** (séparé de `dash_overrides.toml`, manuel), compilation (stubs
  d'imports inconnus générés dans `src/auto_stubs.cpp`), test de fumée Xvfb + lavapipe (capture, aucun FATAL).
- Critère rempli : 6770 régénéré par l'outil (`dashboards/2.0.6770.0-regen/`, non versionné) = mêmes 24 669 fonctions,
  code généré identique fonction par fonction, compilation OK, capture équivalente, aucun FATAL. Le projet 6770
  lui-même a été refactoré sur le gabarit (`CMakeLists.txt` générique : tous les `src/*.cpp`), ses overrides
  automatiques déplacés dans `overrides.toml` ; seuls `diag_hooks.cpp` (+ hooks) et `version_stubs.cpp` restent propres
  à 6770. Les 5 dispatchers à `end` manuel n'étaient pas nécessaires.
- Reste : les modules flash d'une version viennent d'une archive XeBuilds (`--flash-dir`) car la mise à jour ne
  contient souvent que des deltas `.xexp` (pas d'application de delta) ; la liste d'alias de `xzp_alias.py`
  (`DASH_6770`) et les stubs/ordinaux XAM propres à une version restent à vérifier par version (P3) ; pas de
  récupération automatique des fonctions dont le funclet `catch` retourne dans la fonction parente.

### P3 — Portage des versions Blades (FAIT en partie)
- **6717 : OK** (Games + profil, Achievements, Played Games, captures dans `dashboards/2.0.6717.0/runs/preuves/`).
- **7258 : partiel** : démarre, interface en tuiles (NXE avant la lettre) et profil détecté ; il a fallu un correctif du
  SDK (imports sans thunk, adresse 0 dans `PPCFuncMappings`). Achievements/Played Games non atteints (écran de
  profil derrière « Sign In »).
- **5759 et 4532 : partiel** : codegen et compilation OK, écran noir (déréférencement nul à l'initialisation XUI,
  voir README). Pistes : tracer les appels XAM/noyau avant le crash et les comparer à 6717 ; ces versions ouvrent
  `media:\gamercrd.xzp` (ressource XAM de la même version, indisponible : XAM base 1888 chiffrée).
- **2255 : impossible** (que des deltas `.xexp` sur la base 1888).
- Outil : continuations de funclets (`finally`) déclarées automatiquement, copie du profil de test et de
  `input_test.sh`, extraction de `gamercrd`.
- Reste : débloquer 5759/4532 ; atteindre le profil/succès sous 7258 ; modules flash XeBuilds manquants pour 5759/4532
  (on emprunte ceux de 6717) ; regénérer 6770 avec l'outil pour y ajouter d'éventuelles continuations de funclets.

### P4 — Versions avec XAM recompilé (17559 d'abord) (EN COURS : palier 1 FAIT, palier 2 prototype)
- Dashboard 17559 + XAM 17559 recompilés ; XAM comme module ReXGlue (`[[modules]]`), ses exports
  remplaçant ceux du HLE ; démarrage de XAM (threads système, notifications, Guide).
- Les remplaçants ImGui (`dash_xam_ui.cpp`) ne servent plus pour ces versions.
- **Fait** : Dashboard 17559 porté par l'outil (`dashboards/2.0.17559.0/`, 38 105 fonctions, compile, démarre ; 3 correctifs
  `common/` : image en lecture/écriture, `KeGetImagePageTableEntry`, `KeQueryInterruptTime`) ; XAM 17559 recompilé
  (`tools/codegen_module.py`, 19 972 fonctions, `libdash_xam.so`) et **chargé avec son point d'entrée dans le même processus
  que le Dashboard** (`dashboards/2.0.17559.0-xam/`, `DASH_XAM_MODULE=1|2`) ; correctif SDK (`libXXX.so` sous Linux).
- **Palier 1 FAIT (2026-11-06)** : l'interface Metro 17559 s'affiche en HLE (home/games/apps/settings, navigation clavier ;
  captures `dashboards/2.0.17559.0/runs/preuves/p4_*.png`). Cause du « fence 0x0D jamais publié » : `KeInsertQueueDpc` ne
  dispatchait pas (le D3D 17559 soumet ses tampons imbriqués par une DPC) ; correctif SDK `8eb253f`. Autres correctifs :
  `setjmp`/`longjmp` du CRT déclarés (Lua), stubs XAM qui renvoyaient leur 1er argument (langue, contexte de Dashboard,
  Kinect...), ressources XZP embarquées dans `dash.xex` extraites par l'outil (`langpack://` -> `media:\<nom>.xzp`),
  `XeCryptHmacSha*`. Profil du hub résolu en P4b (`common/src/dash_user.cpp`) ; reste `XLIVEBASE 0x58003` (échec conservé, voir README).
- **Bloqué (palier 2)** : le premier thread système de XAM lit un singleton nul (périphériques `Harddisk0\Partition1`...
  absents) ; la redirection des imports du Dashboard vers les exports de XAM n'est pas écrite.
- **Suite chiffrée** (détail et risques dans le README, « Dashboard 2.0.17559 et XAM recompilé ») : 2) XAM vivant
  (périphériques, stubs noyau), 2-3 j ; 3) générateur de redirections + `XamShow*UI`, 1-2 j ; 4) profil/succès via XAM,
  3-5 j ; 5) rendu XUI de XAM + manette, 2-4 j ; 6) retrait d'ImGui, captures, AppImage, 1 j. À court terme : profil et
  Games/My Games du 17559 en HLE (comme 6770), puis AppImage Metro.
- **NXE 2.0.13604 (HLE, FAIT en partie, 2026-11-06)** : interface NXE affichée et pilotée au clavier, AppImage OK
  (`tools/xex_rekey.py` : le `xam.xex` 13604 est chiffré avec la clé XEX1 ; polices 6770 en substitution) ; profil, Recent Games, Achievements, disque et
  lancement/retour de Skate **faits** (P4b, 2026-11-12 ; reste le bouton Play Game du hub, non câblé), puis recompilation de XAM 13604 (XEX désormais décodable).
  Détail : README, « Dashboard NXE 2.0.13604 ». **Découverte** : les `xam_*.xex` Blades de XeBuilds (6717, 7258...) se
  déchiffrent aussi avec la clé XEX1 : XAM Blades recompilable (à explorer ; 6770 reste base 1888 + delta).
- Ensuite : autres mises à jour complètes NXE/Metro.

### P5 — Lancement des jeux (FAIT)
- Registre `games.toml` (`<game_data_root>/games.toml`, modèle `games.example.toml`) : TitleID -> nom + `native`
  (commande + `cwd`) ou `xenia` (commande Xenia : `--dash_xenia`, `DASH_XENIA`, `[xenia] command`).
- Hooks `XamLoaderLaunchTitle` / `XamLoaderLaunchTitleEx` (`common/src/dash_launch.cpp`) ; lanceur ImGui F5 dans le
  Dashboard ; `--dash_launch=<TitleID>` pour les tests.
- Mécanisme : le Dashboard lance un superviseur détaché (`sh`) et quitte ; le superviseur exécute le jeu puis relance
  le Dashboard avec les mêmes arguments (`DASH_RETURNED_FROM`). Pas de partage de fenêtre/GPU/audio.
- Critère rempli : Skate (Skate1Recompiled) lancé depuis le Dashboard 6770 (sous Xvfb + lavapipe : écran-titre
  rendu), puis retour au Dashboard ; idem avec une commande factice. 6717 recompilé, smoke OK.
- **P5b (FAIT)** : lancement natif depuis les boutons du Dashboard 6770 : le bouton de disque (jeu `disc = true`) affiche le
  nom et l'icône du jeu et A le lance ; Games Library > « My Games » liste les jeux du registre (`library = true`), « Last
  Played Game » et la page « My Games - <jeu> » avec « Play Game » les lancent (voir README, « Bouton de disque et My
  Games »). Disque : notification `0x8000000D` (type de média 1), XEX synthétique monté en `\Device\Cdrom0`, message
  XAM `0xFB/0xB003C` (nom + icône), `XamLoaderGetMediaInfo`. My Games : `XamContentGetAttributesInternal` réussi,
  `XamContentCreateInternal`/`Close` interposés pour monter un `ArcadeInfo.xml` (UTF-16 BE avec BOM) généré,
  `XamContentLaunchImageInternal`. La date « dernier joué » du jeu est mise à jour dans `FFFE07D1.gpd`. Captures :
  `dashboards/2.0.6770.0/runs/preuves/p5b_*.png`.
- Reste : retour sur l'onglet d'origine (le Dashboard repart sur Games) ; lancement depuis Played Games inexistant dans le
  6770 ; jeu absent de `FFFE07D1.gpd` : pas d'entrée créée (date non mise à jour) ; icône sans GPD/SPA ni `icon` : pas
  d'icône ; seul Arcade (`0xD0000`) est géré par « My Games » (démos, Xbox Originals, XNA : autres tâches par type,
  non essayées) ; commande Xenia non testée (Xenia absent) ; autres versions (7258, 5759, 4532) non rebuildées avec le
  code P5b ; entrées du lanceur F5 en ImGui (police par défaut) et non en XUI.

### P6 — AppImage (FAIT pour 6770, 6717 et NXE 13604)
- `tools/make_appimage.sh <version>` : `appimagetool` (téléchargé dans `work/appimage-tools/`), binaire + `librexruntime.so`
  + `librexgpu-xenos.so`, `AppRun`, `.desktop`, icône originale ; aucun fichier Microsoft embarqué.
- Premier lancement : import de la mise à jour officielle (dialogue zenity/kdialog, console ou `--import`) via les
  étapes `extract`/`assets` de `port_dashboard.py` -> `~/.local/share/xenondash/<version>/`. Python 3 système requis.
- Vérifié (`tools/test_appimage.sh`) : 6770, 6717 et 13604 (import du `$flash_xam.xex` de la mise à jour, polices xebuild-6770) sous Xvfb/lavapipe avec HOME temporaire, Dashboard affiché.
- Reste : AppImages 7258/5759/4532 (même outil, non testés), test sur un bureau « vide » (conteneur sans dépendances),
  embarquer libX11/xcb/wayland si une distribution cible ne les fournit pas, dialogue graphique de sélection de
  fichier (paquet STFS : zenity ne sélectionne ici que des dossiers), relance après un jeu sous AppImage (non testée).

### P7 — Rendu natif D3D (jalon 2 partiel, arrêté 2026-11-08 ; converge vers la couche commune de P8)
- `native_gpu/` (bibliothèque OBJECT, liée seulement avec `-DDASH_NATIVE_GPU=ON`) : enveloppeurs de fonctions D3D invitées (symbole fort sur l'alias faible
  `sub_XXXXXXXX`), GPU nul + vblank, Vulkan direct (SDL, sans plugin), shaders SPIR-V de XenosRecomp, textures détuilées. Détail, cartographie confirmée et écarts :
  [docs/rendu_natif.md](docs/rendu_natif.md) §11.
- **Fait (6770)** : runtime sans plugin GPU, `Clear`/`Swap` natifs, dessins `DrawVerticesUP`/`BeginVertices`+`EndVertices`, constantes, textures 2D, blend ; écran Games
  reconnaissable (rendu encore partiel : cibles hors écran, scissor, profondeur, échantillonneurs).
- **Jalon 2 (partiel)** : bug des textures E (+0x1000) corrigé, `DrawIndexedVerticesUP`, outil `tools/framecmp.py`, mode observation ; Games presque identique. Détail : docs/rendu_natif.md §12. Poursuite via **P8** (couche D3D de Skate1Native).
- **Suite (ancien plan)** : cibles de rendu/`Resolve`, états, chemins de dessin restants, vidéo (A7), parité contre l'oracle `xenos`, retrait de l'émulation ;
  puis dérivation par motif de code pour 13604/17559 (+XAM). Estimation restante ≈ 24 j pour 6770, ≈ 65 j au total.

### P8 — Fusion des ports de jeux, recompilation à la demande (PROPOSÉ 2026-11-08)
Idée de l'utilisateur : XenonDash voit le XEX d'un jeu (disque, My Games, Games Library) et, au premier lancement,
le recompile en binaire natif, mis en cache, puis le lance ; les travaux des ports existants (Skate1Recompiled/Skate1Native,
TrialsReVolution : Trials HD/Evolution/Fusion) sont fusionnés dans XenonDash.
- **Recettes par jeu** (`games/<TitleID>/`) : manifeste et overrides de codegen, hooks/correctifs C++ du port, correctifs SDK
  propres au jeu, vérification du XEX (hash/version/TU). Reprises de Skate1Recompiled et TrialsReVolution.
- **Un seul SDK** : le fork `xenondash` absorbe les correctifs SDK des trois projets (ceux d'émulation GPU de Trials deviennent
  inutiles si le rendu natif suffit).
- **Un seul rendu natif** : la couche D3D native de Skate1Native (60 FPS sur Steam Deck, cache de shaders XenosRecomp,
  rejeu de captures) devient la bibliothèque partagée par le Dashboard et les jeux ; `native_gpu/` (P7) converge vers elle ;
  correspondance des fonctions D3D par motif de code pour chaque XEX.
- **Recompilation à la demande** : XenonDash lance `codegen` + compilation (conteneur podman/docker comme Skate, ou
  clang hôte), écran de progression, cache `~/.local/share/xenondash/games/<TitleID>/<hash>/`, puis le lanceur P5.
  Jeu sans recette : tentative générique (boucle automatique de P2), sans repli sur l'émulation.
- **Contraintes** : temps de compilation (dizaines de minutes), espace disque et mémoire (Skate : 40 Go, 16 Go de RAM),
  chaîne de compilation chez l'utilisateur ; licences (Skate1Recompiled GPL-3.0 → XenonDash en GPL-3.0 ;
  vérifier TrialsReVolution et ses tiers) ; aucun fichier de jeu ni binaire distribué.
- **SDK unifié et XenosRecomp unifié FAITS (2026-11-12)** : branche `unified` du fork SDK (11 commits : Skate1Native/Trials `linux-android/0001..0008`,
  conflit `mnk_input_driver` résolu en gardant les deux comportements, D3D12 écarté) ; Dashboards 6770, 6717, 13604, 17559 construits et testés
  (smoke) contre ce SDK ; sous-module et `sdk/` basculés sur `unified`. XenosRecomp : série `patches/xenosrecomp/` + `tools/build_xenosrecomp.sh`,
  70/70 shaders UI identiques. Détail et reste (isolation de l'émulation GPU, Skate/Fusion non rejoués) : [docs/fusion_ports.md](docs/fusion_ports.md) §8.
- **Noyau de rendu commun FAIT (2026-11-12, étape 3)** : `ngpu_core/` (moteur de Skate1Native sur plume/Vulkan, `ShaderProvider`, `Profile`, `Core`), façade D3D 6770 (`native_gpu/src/front6770.cpp`, option `NGPU_CORE`, défaut ON avec `-DDASH_NATIVE_GPU=ON`) :
  écran Games à **3,4/255, 32,8 dB** de l'oracle `xenos` (plancher de bruit 1,8/255, 36 dB ; ancien backend 14,3/255, 22,9 dB), artefacts (lignes x≈235/1040, arcs, calques) disparus : l'ombre du périphérique 6770 est celle de Skate.
  Reste : ImGui en natif, MSAA, rampe d'affichage réelle, présentation sans copie CPU, façade Skate et profil de recette (étape 4), 13604/17559. Détail : [docs/ngpu_core.md](docs/ngpu_core.md).
- **Moteur de recettes et Skate en recette FAITS (2026-11-12, étape 4)** : `games/45410813/` (verrou SHA-256 de l'image XEX décodée, analyse, hooks, profil D3D, façade sur `ngpu_core`, GPU nul de Skate),
  `tools/recipe_build.py` (identification, cache `~/.local/share/xenondash/games/<TitleID>/<clé>/`, codegen, shaders par XenosRecomp unifié, compilation clang hôte, test d'amorcage).
  Skate rendu **en natif par `ngpu_core`, sans plugin xenos** (écran-titre sous Xvfb/lavapipe, ≈ 40-50 images/s) ; cache de shaders **identique octet pour octet** à celui de Skate1Native (348 shaders) ;
  depuis le Dashboard 6770 : disque simulé « Skate » -> construction à la demande (fenêtre de progression ImGui) ou cache -> Skate natif -> retour. Détail, mesures et reste : [docs/recettes.md](docs/recettes.md).
- Inventaire fait (étape 1) : [docs/fusion_ports.md](docs/fusion_ports.md) (SDK, licences, rendu natif de Skate, recette, plan chiffré ≈ 62-76 j).
- Étapes : (1) inventaire des correctifs des trois projets et de leurs licences ; (2) SDK unifié ; (3) extraction du rendu
  natif de Skate en bibliothèque, Dashboard 6770 dessus (FAIT, voir ci-dessus) ; (4) format de recette + Skate porté en recette, compilé et lancé
  depuis le Dashboard (FAIT) ; (5) Trials ; (6) recompilation générique.

## Risques connus
- Rendu logiciel (lavapipe/Xvfb) qui se fige sur certaines transitions : tests sur vrai GPU requis.
- Exceptions C++ du Dashboard fatales (`RtlUnwind` non implémenté dans le SDK) : à implémenter dans le
  fork si elles se multiplient.
- Stubs du SDK qui renvoient leur premier argument : à auditer (fonctions booléennes `XamIs*` surtout).
- Le binaire recompilé contient une traduction du code Microsoft : AppImages pour usage personnel,
  non redistribuables.
