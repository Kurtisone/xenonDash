# Reprise du projet par une autre session ou un autre modèle

Ce document permet de reprendre le travail sans l'historique de conversation. Dernière mise à jour :
2026-11-21 (nuit). **État bref** : boot propre, hub NXE navigable au clavier, Game Library stable
(entrée/sortie sans crash), boîtes système rendues, disque complètement supprimé, une seule AppImage
`XenonDash-Kinect.AppImage`. **Front unique : le Guide s'ouvre et sa scène 0x21015 CHARGE (r3=0, au
banc et sur la machine de l'utilisateur) mais ne prend pas encore la présentation** — détails dans
« État du front », mesures complètes dans `docs/xam_13604_compilation.md`. Index de la documentation :
[INDEX.md](INDEX.md) ; le README est destiné aux utilisateurs.

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
  et `rsvg-convert` (paquet `librsvg2-bin`).
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
  manifeste ou d'overrides), puis `python3 tools/xam_unimpl_patch.py`, puis compilation.
- Builds longs en arrière-plan avec attente BORNÉE ; jamais deux builds lourds simultanés ; un seul
  agent par groupe de fichiers.

## Pièges connus
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
- **Touches : le mappage réel (`mnk_input_driver.cpp` du SDK)** est `A = Espace ou ';'`,
  `B = Backspace ou apostrophe`, `LB/RB = 1 / 3`, `Back = Z ou Tab`, `Start = Entrée ou X`,
  stick droit = flèches, D-pad = `Shift`+flèches. `Entrée` est **Start** et `Échap` n'est lié à rien.
  Avant de conclure à un gel, vérifier qu'une autre touche change l'image.
- `tools/dash_shaders.py` attend la sortie de `xxhsum` ≤ 0.8.1 ; xxhash ≥ 0.8.2 écrit autrement.
- Glyphe `U+E00A` (Gamerscore) : absent des trois `.xtt` fournis — carré vide dans le bandeau.
  Voir `docs/technique.md`, « Non résolu ».
- Le fork du SDK est le sous-module `third_party/xenondash-sdk` (branche `unified`) ; après
  modification : reconstruire, réinstaller dans `sdk/`, commiter le sous-module PUIS le dépôt.
- Les instrumentations dans le code GÉNÉRÉ (`dashboards/*/generated/**`) sont non versionnées et
  perdues à la régénération — y mettre des sondes temporaires seulement ; le correctif durable va
  dans `common/src`, `native_gpu` ou le SDK.

## Profil (palier N4, commité)
`DASH_PROFILE_MODE=xam` (activé par l'AppImage) : création de profil, connexion et parcours « aucun
profil » avec les vraies fenêtres Xbox. Pilote STFS minimal côté noyau, profil local hors ligne (XUID
aléatoire, clé locale tirée au hasard), aucune clé de console. `dash_auto_profile` (défaut `false`) :
sans profil, `XamUserGetName`/`GetGamerTag` renvoient une chaîne vide avec succès (jamais d'erreur :
le Dashboard lirait un tampon non initialisé) — le bandeau affiche le score seul. Aucun hook de
*création* de profil côté hôte : le marqueur `profiles/<XUID>/.created` reste à poser quand le
parcours XAM le permettra. Détail : `docs/xam_recompile.md`.

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
- **Restant : la présentation.** La scène chargée ne prend pas le rendu ; le compositeur affiche
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
- **`XeKeysGetStatus`** : remplir la sortie (`*r3 = 0xC`) — sinon XAM ouvre la boîte modale du boot
  (« Game Error »/« Unrecognized Disc »).
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

## Suite, par ordre de priorité
1. **Guide — couche présentation** : suivre les notifications post-ouverture (apps 255 et 9,
   `[0x81C0638C]`) pour trouver qui fait présenter la scène chargée ; ensuite la bascule du
   compositeur. Tout le reste de la chaîne fonctionne (voir « État du front »).
2. **Manette** : la chaîne pad→SDL→InputSystem→XInputd→XAM est vivante et SDL voit les deux manettes
   (Steam Deck interne + Afterglow) ; les boutons sont arrivés au hook dans une session (12:56) et
   plus du tout dans une autre (14:20). Diagnostic en place (`XInputdReadState` journalise «
   l'InputSystem n'a pas d'etat ») — trancher avec le FICHIER `dash.log` de l'utilisateur.
3. **Profil** : « Enter Pass Code » inexpliqué ; `XamUserReadProfileSettings(Ex)` redirigé vers XAM
   plante le Dashboard (lecture invitée de `0x7008004F`) — gamertag/Gamerscore du bandeau restent
   liés au profil HLE ; fenêtre « Save Profile » absente du parcours.
4. **Écran de progression des jeux** : les boîtes XAM fonctionnent désormais — brancher la
   progression de construction (`dash_xam_hud_ui.cpp`, socle N5 déjà commité) dessus et vérifier à
   l'écran ; toasts et redirection des succès.
5. **Divers** : glyphe Gamerscore `U+E00A` absent des `.xtt` ; avatar (paquet utilisateur) ;
   recompilateur générique pour XDK tardives (Trials Fusion : image noire, shaders hors
   `Set*Shader`) ; vrai lecteur optique exposé comme `\Device\CdRom0` (directive utilisateur).
