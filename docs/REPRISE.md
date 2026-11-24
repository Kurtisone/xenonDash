# Reprise du projet par une autre session ou un autre modèle

Ce document permet de reprendre le travail sans l'historique de conversation. Dernière mise à jour :
2026-11-23. **État bref** : boot propre, hub NXE navigable au clavier, Game Library stable, boîtes système
rendues, disque complètement supprimé, une seule AppImage `XenonDash-Kinect.AppImage`. Le Guide s'ouvre et
l'utilisateur le voit à l'écran (Deck). Création de profil, connexion par Sign In sans code d'accès et gamertag
sur le XMB fonctionnent (banc). Couleurs et voile du Guide corrigés. **Avatar** : l'index des paquets de XAM est
rempli et le module `Guide.AvatarMiniCreator.xex` est recompilé ; l'écran de création d'avatar s'affiche après la
connexion d'un profil sans avatar (banc), mais l'avatar n'est pas encore vérifié dans le profil ni sur le XMB, et
l'AppImage n'a pas encore été testée sur le Deck avec ce correctif. Détails : « Avatar » ci-dessous ; mesures
complètes dans `docs/xam_13604_compilation.md`. Index de la documentation : [INDEX.md](INDEX.md) ; le README est
destiné aux utilisateurs.

## Objectif en cours (Kinect 2.0.13604)
Le Dashboard Kinect doit rester dans l'interface Xbox : plus de fenêtres ImGui façon Xenia-canary ; sans profil, le même parcours qu'une console
(connexion, création de profil, clavier virtuel, avatar) avec les vraies fenêtres du HUD de XAM. **Rendu natif uniquement, zéro émulation**
(Xenia-canary seulement comme oracle de comparaison, `--gpu_plugin=xenos` idem). Blades (6770) et Metro (17559) : à reprendre quand l'utilisateur le demande.

## Où lire l'état
`git log --oneline | head -40` ; `PLAN.md` ; `docs/xam_recompile.md` (paliers A à N4 : XAM 13604, HUD, modules XUI, entrées, profil) ;
`docs/xam_13604_compilation.md` (journal détaillé de la variante -xam, sections datées) ; `docs/ngpu_core.md` (rendu natif) ;
`docs/nxe_jeux.md` ; `docs/recompilateur_generique.md` ; `docs/bibliotheque.md` ; `docs/technique.md`.
Notes détaillées des agents (non versionnées, `work/notes/`) : `xam13604-AVANCEMENT.md`, `hud_native-…`, `n3_modules-…`, `n4_profile-…` (si présent).

## Environnement de compilation (hors poste de développement)

Sur une machine sans `clang`/`cmake`/`ninja` (SteamOS, `/usr` non inscriptible), tout passe par
**podman**, avec une image dérivée de `localhost/xenondash-sdk-builder` :

```sh
podman build -t xd-builder:latest -f - . <<'EOF'
FROM localhost/xenondash-sdk-builder:latest
USER root
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      libvulkan-dev libxkbcommon-dev xxhash vulkan-tools xvfb mesa-vulkan-drivers \
      librsvg2-bin fuse file gdb strace && rm -rf /var/lib/apt/lists/*
EOF
```

- **Sous-modules** : `third_party/xenondash-sdk` doit être initialisé *avec les siens* :
  `git -C third_party/xenondash-sdk submodule update --init --recursive --depth 1`. Sans eux, le
  configure échoue sur `install TARGETS given target "xxhash"`.
- **SDK** : `cmake -S third_party/xenondash-sdk -B third_party/xenondash-sdk/out/build/linux-amd64 \
  -G "Ninja Multi-Config" -DCMAKE_INSTALL_PREFIX=$PWD/sdk`, puis
  `cmake --build … --config Release --target install` (≈ 10 min, 8 tâches). Après une modification
  du SDK : `cmake --build . --config Release` puis recopier `out/linux-amd64/Release/librexruntime.so`
  dans `sdk/lib/` (le dash lie celle-là).
- **Codegen** : `/src/sdk/bin/rexglue codegen dash_manifest.toml` (≈ 1 min, 26 916 fonctions).
- **Dashboard** : configure dans `dashboards/2.0.13604.0-xam/out/build/linux-amd64-relwithdebinfo`
  avec `-DCMAKE_PREFIX_PATH=/src/sdk` (≈ 8 min).
- **AppImage** : `APPIMAGE_EXTRACT_AND_RUN=1 BUILD_DIR=linux-amd64-relwithdebinfo \
  ./tools/make_appimage.sh 2.0.13604.0-xam` — sortie fixe `out/appimage/XenonDash-Kinect.AppImage`
  (une seule AppImage, demande utilisateur ; le nom ne varie plus). `appimagetool` réclame `file`
  et `rsvg-convert` (paquet `librsvg2-bin`). **Dans le conteneur podman**, monter aussi le dossier
  `work/` du dépôt principal : `work` est un lien vers `/home/kurtisone/Projects/xbox360/work`, et sans ce
  montage l'étape d'empaquetage échoue (« cannot create directory … work »). Les modules recompilés
  embarqués sont listés par `module recompilé embarqué : libdash_*.so` dans le journal de build.
- **Fichiers absents du dépôt, nécessaires au build** : `dashboards/2.0.13604.0-xam/assets/sep/Guide.AvatarMiniCreator.xex`
  (copie de l'XEX de la mise à jour, ignorée par git via `*.xex`). Sans lui, le module d'avatar n'est pas compilé.
- **Validation minimale du XMB (procédure éprouvée, 2026-11-22)** : lancer `Xvfb :N -screen 0 1280x720x24
  -fbdir <dossier> &`, puis le dash avec `DISPLAY=:N DASH_AUDIO=nop DASH_XAM_MODULE=2 DASH_PROFILE_MODE=xam
  VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json LD_LIBRARY_PATH=<sdk>/lib
  --game_data_root=~/.local/share/xenondash/2.0.13604.0-xam --gpu_plugin=xenos` (en arrière-plan).
  Attendre **60 s**, puis `xdotool windowfocus $(xdotool search --onlyvisible . | tail -1)`, attendre
  5 s, capturer avec `tools/xwd2png.py <dossier>/Xvfb_screen0 <out.png>` et lire l'image. Sans le focus
  ou avant 60 s, la capture est noire même pour le build de référence. Résultat attendu : XMB (Xbox LIVE,
  My Xbox, Welcome, Getting Started, Kinect for Xbox 360, Family First). Vérifié le 2026-11-22 : build de
  référence `out/build/native/dash` et build du fork (`xenondash-sdk` f2c7948) donnent la même capture.
- **Banc visuel (conteneur)** : `Xvfb :N -screen 0 1280x720x24 -fbdir <dossier>` (créer le dossier
  et `/tmp/.X11-unix` chmod 1777 au préalable), capture par `tools/xwd2png.py <fbdir>/Xvfb_screen0
  <out.png>`, `VK_ICD_FILENAMES=…/lvp_icd.json` (lavapipe). Le GPU réel ne peut pas présenter sur
  Xvfb (`No DRI3 support`) et la capture de la session réelle (`:0`) est noire (KWin composite).
  Focus obligatoire : `xdotool windowfocus $(xdotool search --onlyvisible . | tail -1)` (sans WM,
  les touches tombent dans le vide). Pour reproduire la machine de l'utilisateur, monter ses VRAIS
  jeux (`-v /home/deck/XenonDash/Games:/games:ro`, `XENONDASH_GAMES_DIR=/games`) : une entrée de
  registre synthétique sans fichiers provoque un crash memset artéfact au premier present.
- **Sous gdb** : `handle SIGSEGV nostop noprint pass` EST OBLIGATOIRE — sinon gdb intercepte les
  fautes avant le récupérateur du SDK (`[rec2]`, restauration de page commitée) et fabrique des
  crashes imaginaires (vu sur le remplissage d'image du hub, `base+0xFE…` : n'existe pas hors gdb).

## Règles de l'utilisateur
- **Lecteur CD vide et muet** : la simulation de disque est **supprimée entièrement** (2026-11-21) —
  plus de `disc = true` (clé ignorée), plus de `--dash_disc`, plus de XEX synthétique ;
  `XamLoaderGetMediaInfo` → « aucun média », `XamLoaderGetDvdTrayState` → 2 (fermé sans disque).
  Le lecteur ne réagira qu'à un vrai lecteur optique (à faire). Les jeux passent par le dossier
  monté (bibliothèque).
- Commits **uniquement sous son nom** (Kurtisone, noreply GitHub), **jamais de Co-Authored-By ni de
  lien de session**. Pousser sur `forgejo` ; le sous-module SDK d'abord s'il change.
- Aucun fichier Microsoft, de jeu, shader traduit, capture ou binaire dans le dépôt (`generated/`,
  `out/`, `work/` sont ignorés).
- Pas de clés de console fabriquées, pas de contournement de protection de contenu : profil local
  hors ligne avec données de test.
- Confirmer avant toute action destructrice ou sortante (force-push, suppression, publication).

## Méthode qui a marché
- Avant de coder : établir la cause avec preuves (gdb en batch, watchpoints sur les alias mémoire,
  journaux avec `--log_flush_interval=1`, traces par `REX_HOOK_RAW`). Le code PowerPC se lit avec
  `tools/ppc_dis.py` ; les fonctions D3D se retrouvent par motif avec `tools/d3d_match.py`.
- **Chaque hypothèse doit être mesurée avant d'être annoncée, et une hypothèse réfutée doit être
  retirée du dossier** (des sessions entières ont été perdues sur des hypothèses non réfutées à temps).
- Vérifier chaque résultat visuel par **capture regardée** (Xvfb + lavapipe), jamais par le seul
  journal. Écrans X dédiés par tâche (:92, :94, …) ; `ulimit -c 0` avant dash ; `XKEYS_HOLD=0.12`.
- Shaders du rendu natif : `DASH_NGPU_SHADERS`, `DASH_HUD_SHADERS` (sinon `<game_data_root>/shaders/{dash,hud}`,
  générés à l'import de l'AppImage). **Tous traduits** : 258/258 dash et 34/34 HUD (correctifs
  `patches/xenosrecomp/0005` à `0008` + `ngpu_core/src/renderer.cpp` ; détail dans `docs/technique.md`).
- Régénérer les modules XAM/HUD : `python3 tools/regen_xam_modules.py` (après tout changement de
  manifeste ou d'overrides), puis compilation. `tools/xam_unimpl_patch.py` est déjà lancé par
  `regen_xam_modules.py` (inutile de le relancer à la main).
- Builds longs en arrière-plan avec attente BORNÉE ; jamais deux builds lourds simultanés ; un seul
  agent par groupe de fichiers.

## Pièges connus
- **« Enter pass code » après redémarrage** : `signin.xex` compare `RtlCompareMemoryUlong` à 4. Une bibliothèque
  `librexruntime.so` sans le correctif SDK `7151265f` (comptage par octets) renvoie 1 et fait demander un code.
  Le binaire a `RUNPATH=$ORIGIN` sans bibliothèque à côté : il prend celle de `LD_LIBRARY_PATH`. Vérifier la
  bibliothèque utilisée avant de conclure à une régression. Supprimé le 2026-11-23 :
  `third_party/xenondash-sdk/out/linux-amd64/librexruntime.so` (ancien). La bonne est dans `out/linux-amd64/Release/`.
- **Journal du Guide** : `DASH_GUIDE_TRACE=1` active le diagnostic de l'ouverture du Guide (13 lignes par ouverture,
  absentes par défaut).
- Tas invités 0x8… et 0x9… : deux vues du même fichier (alias) ; le SDK sépare les tas par défaut
  (`REX_ALIAS_XEX_HEAPS=1` rétablit l'ancien mode). Table de fonctions d'un module : une
  `function_table_base` par module, sans recouvrement en adresses PHYSIQUES.
- **`dash.log` n'est pas stdout.** `[ngpu]`, `[xinputd]`, `[uistate]`… (REXLOG) vont dans `dash.log` ;
  les `fprintf(stderr)` des instrumentations du code généré vont sur stderr. Lire les deux
  (`> run.out 2>&1`). Un collage de CONSOLE de l'utilisateur ne montre jamais les `[xinputd]` :
  demander le FICHIER `~/.local/share/xenondash/2.0.13604.0-xam/dash.log`.
- `pgrep -f`/`pkill -f` se retrouvent eux-mêmes quand le motif figure dans la ligne de commande
  (même avec des crochets, via d'autres segments du `bash -c` englobant) : tuer par nom exact
  (`pkill -9 -x dash`) ou par PID. Toute attente `while pgrep…` doit avoir un délai maximal.
- Plusieurs instances de dash simultanées : les touches xdotool vont à la mauvaise fenêtre (vérifier
  `pgrep -c -x dash` avant d'interagir) et N instances × ~2 Go d'arène invité déclenchent l'OOM
  killer (le run gdb meurt en SIGKILL). Purger avant chaque lancement.
- **Touches : le mappage réel est celui du Dashboard (`common/src/dash_input.h`,
  `ApplyDashKeyboardLayout`), qui remplace les touches par défaut du SDK** : `A = Entrée ou Espace`,
  `B = Échap ou Backspace`, `Start = P`, `Back = Tab`, `LB/RB = 1 / 3`, `LT/RT = ...` (voir le
  commentaire de `dash_input.h`), flèches = croix. `Entrée` envoie donc **A**, pas Start.
  Sur le clavier virtuel, Start valide « Done » (`P`) : vérifié sur `s2_create` (touche `p`).
  Avant de conclure à un gel, vérifier qu'une autre touche change l'image.
- `tools/dash_shaders.py` attend la sortie de `xxhsum` ≤ 0.8.1 ; xxhash ≥ 0.8.2 écrit autrement.
- Glyphe `U+E00A` (Gamerscore) : présent dans `XenonSCLatin.xtt` (13604), absent de `XenonJKLatin` et
  `XenonCLatin` (6770). Résolu par `src/dash_font_patch.cpp` (police principale JKLatin → SCLatin ; `DASH_FONT_PATCH=0`
  désactive). Voir `docs/technique.md`, « Glyphe Gamerscore ».
- Le fork du SDK est le sous-module `third_party/xenondash-sdk` (branche `unified`) ; après
  modification : reconstruire, réinstaller dans `sdk/`, commiter le sous-module PUIS le dépôt.
- Les instrumentations dans le code GÉNÉRÉ (`dashboards/*/generated/**`) sont non versionnées et
  perdues à la régénération — y mettre des sondes temporaires seulement ; le correctif durable va
  dans `common/src`, `native_gpu` ou le SDK.

## Profil (palier N4)
`DASH_PROFILE_MODE=xam` (activé par l'AppImage) : création de profil, connexion et parcours « aucun
profil » avec les vraies fenêtres Xbox. Pilote STFS minimal côté noyau, profil local hors ligne (XUID
aléatoire, clé locale tirée au hasard), aucune clé de console. Validé (banc, 2026-11-23) : création,
connexion par Sign In sans code d'accès, gamertag sur le XMB (`XamUserReadProfileSettings` synchronisé avec
le profil HLE, cd7f3b0). `dash_auto_profile` (défaut `false`) : sans profil, `XamUserGetName`/`GetGamerTag`
renvoient une chaîne vide avec succès. Les profils de test se suppriment dans `profiles/<XUID>` et
`Content/<XUID>`. Détail : `docs/xam_recompile.md`.

## Avatar (2026-11-23)
- **Cause de l'absence d'écran d'avatar** : `signin.xex` n'appelle `XamShowAvatarMiniCreatorUI` que si
  `XamPackageManagerGetFileSize("Guide.AvatarMiniCreator.xex")` renvoie une taille non nulle. XAM lit son index
  des paquets dans `system.manifest` (signé) de la partition système étendue `\Device\Harddisk0\SystemExtPartition\`,
  qui n'existait pas. Correctifs (commits `66ecb90`, `4af0231`, `3f234c2`, `16251b5`) :
  - `common/src/dash_xam_runtime.cpp` : `MountSystemExtPartition` (monte `<données>/SystemExtPartition` sur
    `\Device\HostSystemExtPartition`, lien depuis `\Device\Harddisk0\SystemExtPartition` : le NullDevice du SDK
    masquerait un montage direct) ; `XboxKrnlVersion` reprend la version du Dashboard (XAM compare la version du
    manifeste à celle du noyau) ; le hook `XexLoadImage` refuse les modules non recompilés de cette partition.
  - `common/src/xam_kernel_stubs.cpp` : `XexLoadImageHeaders` lit l'en-tête XEX2 ; `XeKeysVerifyRSASignature`
    renvoie TRUE en dur (les clés Microsoft ne sont pas disponibles ; le SHA-1 du contenu reste vérifié par XAM).
    **Compromis accepté par l'utilisateur.**
  - `dashboards/2.0.13604.0-xam` : module `Guide_AvatarMiniCreator` (base 0x90100000, table 0x84F00000,
    `guest_path = \sep\20352400\Guide.AvatarMiniCreator.xex`), génération à part dans `tools/regen_xam_modules.py`.
  - `tools/appimage/xenondash_import.py` : `build_system_ext` reconstruit `SystemExtPartition` depuis le dossier
    `$SystemUpdate` dézippé (`AppRun` dézippe le zip de la mise à jour) : `system.manifest` à la racine ;
    `FFFE07DF0000000{1,2,6,8}` dans `Content/0000000000000000/FFFE07DF/00008000/` ; `Xna_TitleLauncher.xex` dans
    `32000100/` ; les autres `.xex`, `.lex`, `.xzp` et `su20076000_*` dans `20352400/`. Vérifié : l'arbre obtenu
    depuis `~/dash/$systemupdate` est identique à l'arbre validé à la main. `--system-ext <dossier>` remplace
    cette reconstruction.
- **Import et reprise** : `build_system_ext` construit la partition dans `<dst>.new` puis la remplace (échec atomique,
  pas de suppression avant vérification). Une installation antérieure sans partition est réparée par
  `AppRun --system-ext-only` (cherche la mise à jour à côté de l'AppImage). Un paquet STFS `su20076000_*` seul ne
  contient pas `system.manifest` : la partition est alors construite depuis le dossier qui le contient.
- **Refus d'alias** : le module d'avatar est reconnu sous `\sep` et sous `\Device\Harddisk0\SystemExtPartition`
  (non testé à l'exécution avec un alias réel).
- **Personnalisation, option 2 (2026-11-23)** : `XamLaunchAvatarEditor` (ordinal 2011, relance du titre
  `AvatarEditor.xex`, 15 Mo, non recompilé) ouvre à la place le choix d'avatar (`XamShowAvatarMiniCreatorUI(user, 2)`,
  ordinal 1700). Point unique : `AvatarEditorOption2` dans `common/src/dash_xam_redirect.cpp` (appelé par
  `CallModuleImport` pour signin, createprofile, gamerprofile, hud, le MiniCreator, et par l'accroche de dash.xex).
  Depuis le MiniCreator (Y), l'ouverture est différée (fil invité de l'hôte) jusqu'au déchargement du module : un appel
  synchrone après son `XamAppUnloadStack` renvoie 5. Validé (banc) : profil créé, Y rouvre le choix, sélection ->
  `XamAvatarSetManifest`, réglage `0x63E80044` (1000 octets) écrit dans `profiles/<XUID>/FFFE07D1.gpd` ; après
  redémarrage, Sign In ne redemande plus d'avatar. **Option 1** (éditeur complet recompilé) : retirer l'appel à
  `AvatarEditorOption2` ; l'import retombe sur l'export 2011 de XAM recompilé.
- **Non vérifié** : le XMB affiche une silhouette grise à la place de l'avatar choisi (rendu d'avatar non étudié) ;
  « Customize Avatar » de dash.xex n'atteint pas `XamLaunchAvatarEditor` (`XamPackageManagerGetExperienceMode`
  du HLE sans le bit 0x2) ; l'AppImage avec ce correctif sur le Deck, à partir d'un premier lancement avec le zip.
- **Régression corrigée puis validée** : après connexion, le Guide affichait un panneau vide (XAM chargeait
  l'XEX d'avatar non recompilé). Corrigé par le refus de `XexLoadImage` ci-dessus ; le Guide revient au menu.
  Reste : après connexion, le Guide se ferme au lieu de rester ouvert (à traiter : faire échouer
  `XamShowAvatarMiniCreatorUI` dans `dash_xam_redirect.cpp`, maintenant que le module est recompilé).

## État du front (Guide)
Chaîne complète établie et sondée (`[guidetrace]`, `[guideio]`, `[scene]`, `[uistate]`, `[app254]`) :
G → portier `sub_8189F0D0` accepte → message `0x20004117` à l'app 254 (via `sub_8187B960`) → le
répartiteur du système UI déroule `0x8000000B` → `0x80000007` (objet de requête 0x402E9B80, scène
en +4) → chargeur `sub_8189BE20` (case 20) → ouvreur `sub_818A21B0`.

- **Corrigé (76797c9)** : l'ouvreur exige `UIstate(0x81BFCAE8)+172≠0, +176≠0, +216==0`. `+176` est
  posé par `sub_8189F050(1)`, déclenché par le message **`0x80000012`** (« interface système ON »,
  traité en loc_81894F3C de `sub_81894D50` ; `0x80000011` = OFF) que personne n'émet chez nous.
  Correctif dans `ShowXamGuide` (`dash_xam_redirect.cpp`) : (a) appel DIRECT du gestionnaire
  `sub_81894D50(0x80000012)` via le répartiteur (l'envoi par la file, r3=0, n'est JAMAIS distribué
  par le répartiteur — mesuré) ; (b) pose de `+172=1` (« pipeline d'affichage prêt » : sur console,
  l'init vidéo D3D de XAM le pose — chaîne que le rendu natif ngpu remplace). Résultat : la scène
  charge (`r3=0`), l'état avance (+0=2, +56=1), au banc ET sur la machine de l'utilisateur.
  **RETIRÉ depuis** (mesure du 2026-11-22, commentaire de `ShowXamGuide`) : plus d'envoi de `0x80000012` à la main
  ni de `+172` forcé ; avec `XeKeysGetStatus` = 0x4, le fil du HUD de XAM (`sub_818A2F90` → `sub_818A2C98`) pose
  lui-même `+172`/`+176`. Ce paragraphe ne décrit plus le code actuel.
- **Présentation (ancienne impasse, 2026-11-23)** : l'utilisateur signale que le Guide s'affiche sur le Deck
  avec le build de la période (cause précise non relevée dans ce document ; les mesures ci-dessous datent du
  21 novembre). Ce qui était mesuré alors : la scène chargée ne prend pas le rendu ; le compositeur affiche
  toujours le hub. Mesuré et écarté : le rappel `[0x402E9B80+48]` n'est jamais écrit, mais la pompe
  UI (`sub_818905B0`) saute ces items (état 2, zéro `[trap]`) — normal ; forcer la cellule « app
  active » (0x817E4B64) à 254 ne change rien à l'affichage — ce n'est pas le sélecteur de rendu.
- **Piste suivante** : les notifications post-ouverture émises par l'ouvreur (`sub_818C54E0(255,1)`,
  `sub_8187E240(9,1)`, écriture `[0x81C0638C]=1`) et le chemin de présentation des apps UI de XAM
  (contextes +8=0x3002B000) — qui appelle VdSwap pour une scène XAM (au repos, seul le dash
  présente, `[swap] lr=0x9218ED74`) et sur quoi le compositeur base la bascule.

## Correctifs récents (une ligne chacun, détail dans le journal git et `docs/xam_13604_compilation.md`)
- **Trap non fatal** (SDK `function_dispatcher.cpp`, f2c7948) : un appel indirect invité vers 0
  (vtable jamais construite, ex. destructeur de la tâche Game Library `sub_922AA048`) avortait tout
  le processus — c'était le « freeze » de la sortie de Game Library. Journal borné + `r3=0`.
- **`NicGetLinkState`** : ne JAMAIS écrire à travers r4 (l'appelant réseau passe un r4 hérité non
  commité) — état « pas de lien » par r3=0 seulement.
- **`XeKeysGetStatus`** : remplir la sortie (`*r3 = 0x4` par défaut, `DASH_XEKEYS_STATUS=<hex>` pour tester) — laissée
  à 0, XAM ouvre la boîte modale du boot (« Game Error »/« Unrecognized Disc ») ; avec 0xC (valeur retail), le fil
  du HUD de XAM ne démarre pas et le Guide ne s'affiche jamais (mesure du 2026-11-22).
- **xam.xex rekeyé aux DEUX emplacements** (flash ET racine des données) : le chargeur résout
  `game:\xam.xex` vers la racine (le montage `game:` du noyau masque la flash) ; code 2 = hash de
  bloc invalide (clé de session fausse), PAS fichier absent.
- **`XamNotifyCreateListener`/`XN_SYS_UI`** : la diffusion hôte est passée en opt-in
  (`DASH_HUD_NOTIFY=1`) — la diffusion automatique reposait le bit « interface active » de XAM.
- **Import hôte** : `xex_rekey.py` sans dépendance obligatoire (AES cryptography → libcrypto → pur
  Python, équivalents octet pour octet) ; polices `.xtt` détectées dans `/home/deck/XenonDash/polices/`.
- **Disque supprimé entièrement** (68c0aa8, d0a352e, SDK d62f5dd) — voir Règles.
- Défauts SDK plus anciens corrigés en chemin : `AccessViolationCallback` refusait `kGuestVirtual`
  (7348c20) ; registre des modules XUI (`game:\flash\<nom>.xex` normalisé) ; `KeSetTimerEx` transmet
  le DPC ; chargement de XAM asynchrone.

## Imports à risque (mesurés le 2026-11-23, comportement inchangé)
- `XeKeysVerifyRSASignature` (TRUE en dur) : 5 appels au démarrage, tous des deux appelants connus
  (0x8185A4C4 et 0x818BF0C0). Restreindre aux deux appelants ne change rien au parcours actuel.
- `XexLoadExecutable` : 1 appel sur le chemin du XMB et du Guide (lr 0x818783AC). **Le nom lu est vide** :
  le hook lit probablement mal ses arguments. Ne pas le faire échouer avant de corriger la lecture.
- `XexStartExecutable` : 1 appel (lr 0x8187A028), entrée non nulle.
- Imports absents (renvoient 0) : aucun appel dans le scénario XMB / Guide / connexion.

## Suite, par ordre de priorité
0. **Connexion sans code** : vérifier sur le Deck avec le build courant qu'un Sign In sur un profil neuf,
   après redémarrage, ne demande pas de code (cause : bibliothèque SDK périmée, voir Pièges).
1. **Avatar** : tester l'AppImage sur le Deck (premier lancement avec le zip) ; corriger la fermeture du Guide
   après connexion (`XamShowAvatarMiniCreatorUI`) ; vérifier que l'avatar choisi est enregistré et s'affiche
   sur le XMB et dans My Xbox ; personnalisation (Y, `AvatarEditor.xex`) à recompiler plus tard.
2. **Manette** : la chaîne pad→SDL→InputSystem→XInputd→XAM est vivante et SDL voit les deux manettes
   (Steam Deck interne + Afterglow) ; les boutons sont arrivés au hook dans une session (12:56) et
   plus du tout dans une autre (14:20). Diagnostic en place (`XInputdReadState` journalise «
   l'InputSystem n'a pas d'etat ») — trancher avec le FICHIER `dash.log` de l'utilisateur.
3. **Profil** : « Enter Pass Code » inexpliqué à vérifier sur le Deck ; sous-menus de Customize Profile
   (image, nom, avatar) non testés ; panneau du Guide vide après B non analysé.
4. **Écran de progression des jeux** : les boîtes XAM fonctionnent désormais — brancher la
   progression de construction (`dash_xam_hud_ui.cpp`, socle N5 déjà commité) dessus et vérifier à
   l'écran ; toasts et redirection des succès.
5. **Divers** : glyphe Gamerscore `U+E00A` (corrigé par `dash_font_patch.cpp`, voir `technique.md`) ; recompilateur générique pour XDK tardives
   (Trials Fusion : image noire, shaders hors `Set*Shader`) ; vrai lecteur optique exposé comme
   `\Device\CdRom0` (directive utilisateur) ; étirement 32:9 de l'AppImage (reporté à la demande de l'utilisateur).
