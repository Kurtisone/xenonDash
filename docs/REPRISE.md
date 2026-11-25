# Reprise du projet par une autre session ou un autre modèle

Ce document permet de reprendre le travail sans l'historique de conversation. Dernière mise à jour :
2026-11-24. **État bref** : boot propre, hub Kinect navigable au clavier, Game Library stable, boîtes système
rendues, disque complètement supprimé, une seule AppImage `XenonDash-Kinect.AppImage`. Le Guide s'ouvre avec ses
onglets (banc). Création de profil, connexion par Sign In sans code d'accès et gamertag sur le XMB fonctionnent
(banc). **Avatar du hub** : le profil choisit un avatar prédéfini, et l'avatar s'affiche en couleur sur le XMB
(banc, commits `243b35e`, `105b468`, `687ecab`, dépôt SDK `69764e99`). **Icône Gamerscore** : corrigée par
`src/dash_font_patch.cpp` (commit `7cf0bd7`). **HUD vérifié au banc** (2026-11-24) : XMB, Guide et clavier
« Enter Profile Name » s'affichent sans carré ni fenêtre tronquée ; boîtes de progression non testées. Non encore
vérifié : l'AppImage sur le Deck avec ces correctifs, et l'éditeur d'avatar complet. Détails : « Avatar » ci-dessous
(voir aussi [avatar_hub.md](avatar_hub.md)) ; compilation dans [compilation.md](compilation.md) ; mesures complètes
dans `docs/xam_13604_compilation.md`. Index de la documentation : [INDEX.md](INDEX.md) ; le README est destiné aux
utilisateurs.

## Objectif en cours (Kinect 2.0.13604)
Le Dashboard Kinect doit rester dans l'interface Xbox : plus de fenêtres ImGui façon Xenia-canary ; sans profil, le même parcours qu'une console
(connexion, création de profil, clavier virtuel, avatar) avec les vraies fenêtres du HUD de XAM. **Rendu natif uniquement, zéro émulation**
(Xenia-canary seulement comme oracle de comparaison, `--gpu_plugin=xenos` idem). Blades (6770) et Metro (17559) : à reprendre quand l'utilisateur le demande.

## Où lire l'état
`git log --oneline | head -40` ; `PLAN.md` ; `docs/xam_recompile.md` (paliers A à N4 : XAM 13604, HUD, modules XUI, entrées, profil) ;
`docs/xam_13604_compilation.md` (journal détaillé de la variante -xam, sections datées) ; `docs/ngpu_core.md` (rendu natif) ;
`docs/nxe_jeux.md` ; `docs/recompilateur_generique.md` ; `docs/bibliotheque.md` ; `docs/technique.md`.
Notes détaillées des agents (non versionnées, `work/notes/`) : `xam13604-AVANCEMENT.md`, `hud_native-…`, `n3_modules-…`, `n4_profile-…` (si présent).

## Environnement de compilation

Voir [compilation.md](compilation.md) : SDK, conteneur, build de test, AppImage.

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
- **Ghidra** : `tools/ghidra/xd_ghidra.py decompile xam <fonction>` donne le C décompilé d'une fonction de XAM ou de
  dash.xex avec les noms partagés de `symbols/2.0.13604.0/` (voir [ghidra.md](ghidra.md)) ; y consigner tout nom établi.
  `tools/function_inventory.py` affiche les mêmes noms.
- **Chaque hypothèse doit être mesurée avant d'être annoncée, et une hypothèse réfutée doit être
  retirée du dossier** (des sessions entières ont été perdues sur des hypothèses non réfutées à temps).
- Vérifier chaque résultat visuel par **capture regardée** (Xvfb + lavapipe), jamais par le seul
  journal. Écrans X dédiés par tâche (:92, :94, …) ; `ulimit -c 0` avant dash ; `XKEYS_HOLD=0.12`.
- `tools/banc.py` : banc pas à pas (`start`, `keys`, `shot`, `wait`, `cpu`, `grep`, `stop`) qui lance la variante -xam
  comme l'AppImage, sur une COPIE du dossier de données (profils de l'utilisateur intacts), écran dédié, arrêt par PID.
  `cpu` donne la charge par fil : écarter les fils `llvmpipe-*` (rendu logiciel de lavapipe).
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
  bibliothèque utilisée avant de conclure à une régression. Les bibliothèques à utiliser sont celles de
  `sdk/lib/` (installation depuis le worktree, 2026-11-24). Ne pas réutiliser une copie hors de ce dossier.
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
  désactive). Voir `docs/technique.md` (puce « Glyphe Gamerscore », section 13604).
- Le fork du SDK est le sous-module `third_party/xenondash-sdk` (branche `dash-13604-recompilateur` pour 13604 ;
  `unified` est plus ancienne) ; après modification : reconstruire, réinstaller dans `sdk/`, commiter le sous-module PUIS
  le dépôt. Le SDK installé est compilé dans le conteneur (clang 20 + libstdc++ 14 : étape SDK de
  `tools/container/in_container.sh`), le Dashboard sur l'hôte. Dans un worktree, `third_party/xenondash-sdk` peut être une
  simple copie sans git : `git init`, puis `git fetch` du commit épinglé ET des étiquettes `v*` depuis un dépôt du SDK qui
  les a (celui du worktree kinect13604 : forgejo n'a pas d'étiquettes), puis `git reset` sur ce commit (les fichiers ne
  bougent pas). Sans les étiquettes, la version devient `0.10.0.0-dev.unknown` au lieu de `0.10.0.N-dev.g<commit>`.
- **Stubs du SDK qui n'écrivent pas leurs sorties** : `REX_EXPORT_STUB` laisse r3 inchangé (souvent le pointeur de
  sortie, donc « succès » pour l'appelant) et n'écrit rien : l'appelant lit des résidus de pile invitée, et tout
  changement de pile (fibres, ordre des appels) change l'écran. Cas résolu le 2026-11-25 : liste des résolutions des
  Paramètres vide (`HalGetCurrentAVPack`, `VdGetDisplayDiscoveryData`, `VdQueryRealVideoMode`, voir
  `dashboards/2.0.13604.0-xam/src/kernel_video_output.cpp`). Devant un écran qui varie d'un build à l'autre,
  chercher d'abord les `STUB` du journal sur le chemin.
- **KeBugCheck est fatal** (SDK `7f60045f`, 2026-11-25) : `[FATAL] *** STOP: 0x<code> (p1, p2, p3, p4), appelant
  lr=0x…, fil …` au niveau critique dans `dash.log`, puis `abort()` (code de sortie -6, SIGABRT). Avant, l'appel revenait
  et le code invité continuait (erreurs secondaires sans lien avec la cause). Le lr est l'adresse de retour de l'appel :
  `python3 tools/ghidra/xd_ghidra.py decompile <module> 0x<lr>` donne la fonction appelante. Le recompilateur termine
  aussi le bloc après un `bl` vers KeBugCheck ou KeBugCheckEx (`return;` dans le code généré) : avant, une fonction
  absorbait le code qui suit l'appel (`sub_819BF2D0`, départ d'une fibre de XAM, contenait une copie non accrochée de la
  bascule de fibre `sub_819BF2E0`).
- Les instrumentations dans le code GÉNÉRÉ (`dashboards/*/generated/**`) sont non versionnées et
  perdues à la régénération — y mettre des sondes temporaires seulement ; le correctif durable va
  dans `common/src`, `native_gpu` ou le SDK.

## Profil, Guide et XAM : état courant

- **Profil** (palier N4) : création de profil, connexion par Sign In sans code d'accès, et gamertag sur le XMB fonctionnent au banc. Profil local hors ligne, aucune clé de console.
- **Guide** : s'ouvre avec ses onglets (banc, 2026-11-24). Chaîne d'ouverture et mesures : [xam_etat.md](xam_etat.md).
- **Imports à risque** : `XeKeysVerifyRSASignature` (TRUE en dur, accepté), `XexLoadExecutable` (nom lu vide : ne pas le faire échouer avant de corriger la lecture). Détail : [xam_etat.md](xam_etat.md).
- **Fibres de XAM** (`src_xam_patch/xam_fiber_switch.cpp`) : `sub_819BF2E0` bascule vraiment de pile hôte (fibre
  ucontext, pile mmap gardée). Sans ce correctif, un fil tourne à 99 % d'un cœur dès le démarrage ; avec, aucun fil
  invité au-dessus de 28 % (banc, 2026-11-25). `DASH_XAM_FIBER_FIX=0` le désactive (comparaison).
- **Correctifs récents** : liste complète dans [xam_etat.md](xam_etat.md) et dans le journal git.

## Avatar

Voir [avatar_hub.md](avatar_hub.md) : chaîne de correctifs, état et réglage du manifeste.

## Suite, par ordre de priorité
0. **Connexion sans code** : vérifier sur le Deck avec le build courant qu'un Sign In sur un profil neuf,
   après redémarrage, ne demande pas de code (cause : bibliothèque SDK périmée, voir Pièges).
1. **Avatar** : l'avatar s'affiche sur le XMB (banc). Reste à vérifier sur le Deck et dans My Xbox.
   Corriger la fermeture du Guide après connexion si elle persiste
   (`XamShowAvatarMiniCreatorUI`). Personnalisation (Y, `AvatarEditor.xex`) à recompiler plus tard.
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
