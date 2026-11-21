# Reprise du projet par une autre session ou un autre modèle

Ce document permet de reprendre le travail sans l'historique de conversation. Dernière mise à jour : 2026-11-21 (le Guide et les boîtes système s'affichent dans la variante -xam ;
AppImage -xam générée et testée ; voir « Environnement », « Pièges » et l'état détaillé dans `docs/xam_13604_compilation.md`, sections 2026-11-20/21). Index de la documentation :
[INDEX.md](INDEX.md) ; le README est destiné aux utilisateurs.

## Objectif en cours (Kinect 2.0.13604)
Le Dashboard Kinect doit rester dans l'interface Xbox : plus de fenêtres ImGui façon Xenia-canary ; sans profil, le même parcours qu'une console
(connexion, création de profil, clavier virtuel, avatar) avec les vraies fenêtres du HUD de XAM. **Rendu natif uniquement, zéro émulation**
(Xenia-canary seulement comme oracle de comparaison, `--gpu_plugin=xenos` idem). Blades (6770) et Metro (17559) : à reprendre quand l'utilisateur le demande.

## Où lire l'état
`git log --oneline | head -40` ; `PLAN.md` (P8) ; `docs/xam_recompile.md` (paliers A à N4 : XAM 13604, HUD, modules XUI, entrées, profil) ;
`docs/ngpu_core.md` (rendu natif du Dashboard et du HUD) ; `docs/nxe_jeux.md` ; `docs/recompilateur_generique.md` ; `docs/bibliotheque.md` ; `docs/technique.md`.
Notes détaillées des agents (non versionnées, `work/notes/`) : `xam13604-AVANCEMENT.md`, `hud_native-…`, `n3_modules-…`, `n4_profile-…` (si présent).

## Environnement de compilation (hors poste de développement)

Sur une machine sans `clang`/`cmake`/`ninja` (SteamOS, `/usr` non inscriptible), tout passe par
**podman**, avec une image dérivée de `localhost/rexglue-sdk-builder` :

```sh
podman build -t xd-builder:latest -f - . <<'EOF'
FROM localhost/rexglue-sdk-builder:latest
USER root
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      libvulkan-dev libxkbcommon-dev xxhash vulkan-tools xvfb mesa-vulkan-drivers \
      librsvg2-bin fuse file gdb strace && rm -rf /var/lib/apt/lists/*
EOF
```

- **Sous-modules** : `third_party/rexglue-sdk` doit être initialisé *avec les siens* :
  `git -C third_party/rexglue-sdk submodule update --init --recursive --depth 1`. Sans eux, le
  configure échoue sur `install TARGETS given target "xxhash"`.
- **SDK** : `cmake -S third_party/rexglue-sdk -B third_party/rexglue-sdk/out/build/linux-amd64 \
  -G "Ninja Multi-Config" -DCMAKE_INSTALL_PREFIX=$PWD/sdk`, puis
  `cmake --build … --config Release --target install` (≈ 10 min, 8 tâches).
- **Codegen** : `/src/sdk/bin/rexglue codegen dash_manifest.toml` (≈ 1 min, 26 916 fonctions).
- **Dashboard** : configure dans `dashboards/2.0.13604.0/out/build/linux-amd64-relwithdebinfo`
  avec `-DCMAKE_PREFIX_PATH=/src/sdk` (≈ 8 min).
- **AppImage** : `tools/make_appimage.sh 2.0.13604.0` — mais il cherche le binaire dans
  `out/build/native/`, alors que le build courant est dans `linux-amd64-relwithdebinfo/` :
  passer **`BUILD_DIR=linux-amd64-relwithdebinfo`**. `appimagetool` réclame en plus la commande
  `file` et `rsvg-convert` (paquet `librsvg2-bin`).
- **Exécution de test** : `Xvfb :N -screen 0 1280x720x24 -fbdir <dossier>`, capture par
  `tools/xwd2png.py <fbdir>/Xvfb_screen0 <out.png>` (ni `ffmpeg` ni `xwd` nécessaires),
  `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json` (lavapipe). Le GPU réel ne peut pas
  présenter sur une surface Xvfb (`vulkan: No DRI3 support detected`).
- La capture X11 de la session réelle (`:0`, Plasma Wayland + Xwayland) est **noire** : KWin
  composite. Vérifier à l'écran passe obligatoirement par Xvfb.

## Règles de l'utilisateur
- **Lecteur CD** : `\Device\CdRom0` reste **vide** (pas de disque) — l'expérience Xbox 360 native, le plus juste possible sans modification. Si la machine possède un vrai lecteur optique, il s'exposerait comme CdRom0 (à faire). Les jeux passent par le dossier monté (bibliothèque existante), pas par le lecteur.
- Commits **uniquement sous son nom** (`git config user.name/user.email` déjà réglés : Kurtisone, noreply GitHub), **jamais de Co-Authored-By ni de lien de session**.
- Aucun fichier Microsoft, de jeu, shader traduit, capture ou binaire dans le dépôt (`generated/`, `out/`, `work/` sont ignorés).
- Pas de clés de console fabriquées, pas de contournement de protection de contenu : profil local hors ligne avec données de test.
- Confirmer avant toute action destructrice ou sortante (force-push, suppression, publication).

## Méthode qui a marché
- Avant de coder : établir la cause avec preuves (gdb en batch, watchpoints sur les alias mémoire, journaux avec `--log_flush_interval=1`, traces par `REX_HOOK_RAW`).
  Le code PowerPC se lit avec `tools/ppc_dis.py` ; les fonctions D3D se retrouvent par motif avec `tools/d3d_match.py`.
- Vérifier chaque résultat visuel par **capture regardée** (Xvfb + lavapipe), jamais par le seul journal. Écrans X dédiés par tâche (:92, :94, ...) ; `ulimit -c 0` avant dash ; `XKEYS_HOLD=0.12` avec le clavier virtuel.
- Shaders du rendu natif : `DASH_NGPU_SHADERS`, `DASH_HUD_SHADERS` (sinon `<game_data_root>/shaders/{dash,hud}`, générés à l'import de l'AppImage).
- Régénérer les modules XAM/HUD : `python3 tools/regen_xam_modules.py` (après tout changement de manifeste ou d'overrides), puis `python3 tools/xam_unimpl_patch.py`, puis compilation.
- Builds longs en arrière-plan avec attente BORNÉE ; jamais deux builds lourds simultanés ; un seul agent par groupe de fichiers.

## Pièges connus
- Tas invités 0x8… et 0x9… : deux vues du même fichier (alias) ; le SDK sépare maintenant les tas par défaut (`REX_ALIAS_XEX_HEAPS=1` rétablit l'ancien mode).
- Table de fonctions d'un module recompilé : `function_table_base` par module, sans recouvrement en adresses PHYSIQUES.
- `/tmp` est un tmpfs (11 Go libres) : caches de jeu et journaux volumineux à supprimer par nom ; core dumps désactivés.
- **`dash.log` n'est pas stdout.** Les lignes du noyau de rendu (`[ngpu] noyau ngpu_core sur …`,
  `[ngpu] Vulkan : …`) et une partie des diagnostics vont sur **stderr**, pas dans le fichier
  `dash.log` de `out/build/…/logs/`. Chercher `ngpu` dans `dash.log` fait conclure à tort que le
  rendu natif ne s'initialise pas : rediriger la sortie du processus (`> run.out 2>&1`) et lire
  les deux. Erreur commise et corrigée le 2026-11-16.
- `pgrep -f` se retrouve lui-même dans une boucle d'attente `while pgrep -f ...` : toute attente doit avoir un délai maximal.
  Même piège avec un `pkill -f <motif>` dont la ligne de commande contient le motif : il se tue lui-même.
- **Touches : ne pas se fier au tableau du README, il a été faux jusqu'au 2026-11-15.** Le mappage
  réel (`mnk_input_driver.cpp` du SDK) est `A = Espace ou ';'`, `B = Backspace ou apostrophe`,
  `LB/RB = 1 / 3`, `Back = Z ou Tab`, `Start = Entrée ou X`, stick droit = flèches,
  D-pad = `Shift`+flèches. `Entrée` est **Start** et `Échap` n'est lié à rien : suivre l'ancienne
  doc donne « rien ne se valide, rien ne se ferme », donc un Dashboard qui **paraît gelé**.
  Une session entière de diagnostic a été perdue sur ce faux gel.
- **Avant de conclure à un gel**, vérifier qu'une autre touche change l'image : un écran de
  contenu sans réaction à `B` alors que `LB`/`RB` fonctionnent n'est pas un gel.
- `tools/dash_shaders.py` attend la sortie de `xxhsum` ≤ 0.8.1 (`XXH3_<hex>  fichier`) ;
  xxhash ≥ 0.8.2 écrit `XXH3 (fichier) = <hex>` (corrigé, mais y penser si un autre outil parse
  cette sortie).
- Glyphe `U+E00A` (Gamerscore) : absent des trois `.xtt` fournis, donc carré vide dans le bandeau.
  Sans rapport avec `shrdres.xzp` (dont le `GScore.png` est valide) ni avec les `.xttp` (jamais
  demandés par la 13604). Voir `docs/technique.md`, « Non résolu ».
- Le fork du SDK est le sous-module `third_party/rexglue-sdk` (branche `unified`) ; après modification : le reconstruire et le réinstaller dans `sdk/` (procédure dans `docs/xam_recompile.md`), puis commiter le sous-module PUIS le dépôt.

## Profil : console neuve (2026-11-16)

`dash_auto_profile` (défaut `false`) : sans profil créé, `XamProfileCreateEnumerator` n'énumère rien
et `XamUserGetName`/`XamUserGetGamerTag` renvoient une **chaîne vide avec succès** (jamais une erreur :
le Dashboard lirait alors un tampon non initialisé et afficherait des caractères poubelle). Le bandeau
affiche donc le score seul, sans nom.

Limite : aucun hook de *création* de profil n'existe (parcours XAM, ci-dessus). Le marqueur
`profiles/<XUID>/.created` n'est donc jamais posé ; poser ce marqueur à la création est le complément
à faire quand le parcours XAM fonctionnera.

## État du profil (palier N4, commité)
Création de profil, connexion et parcours « aucun profil » fonctionnent avec les vraies fenêtres Xbox (`DASH_PROFILE_MODE=xam`, défaut OFF ; variante `dashboards/2.0.13604.0-xam/`) :
pilote STFS minimal côté noyau, profil local hors ligne (XUID aléatoire, signature par une clé locale tirée au hasard), aucune clé de console. Détail : `docs/xam_recompile.md`.

## Suite, par ordre de priorité
1. Profil existant choisi dans la liste : XAM demande « Enter Pass Code » (cause non établie) ; `XamUserReadProfileSettings(Ex)` redirigé vers XAM plante le Dashboard (lecture invitée de `0x7008004F`) : gamertag/Gamerscore du bandeau et contenu restent liés au profil HLE ; fenêtre « Save Profile » absente du parcours Dashboard.
2. Rendre `DASH_PROFILE_MODE=xam` le défaut et livrer la variante `-xam` (XAM + HUD) en AppImage.
   *État au 2026-11-21 : **livrée et vérifiée sur la machine de l'utilisateur** — l'AppRun cherche
   les fichiers de l'import dans le dossier de l'AppImage (`$APPIMAGE`, pas le point de montage :
   la première version cherchait dans le montage et les boîtes de dialogue s'ouvraient malgré les
   fichiers présents), avec notification notify-send au début/à la fin de l'import. Vérifié sur
   Steam Deck : `$SystemUpdate` et xam.xex détectés, import STFS démarré immédiatement, zéro
   dialogue. Historique de l'outillage : — `out/appimage/XenonDash-2.0.13604.0-xam-x86_64.AppImage`
   (142 Mo, outils de recompilation embarqués), construite par `APPIMAGE_EXTRACT_AND_RUN=1 BUILD_DIR=linux-amd64-relwithdebinfo
   ./tools/make_appimage.sh 2.0.13604.0-xam`. Trois pièces ajoutées à l'outillage : (a) les **modules recompilés**
   (`libdash_*.so`) sont embarqués à côté du binaire (le chargeur les cherche à côté de l'exécutable — sans eux,
   retombe en mode HLE, « Registering module » absent, mesuré) ; (b) un marqueur `usr/share/xenondash/xam` fait activer
   par l'AppRun `DASH_XAM_MODULE=2` et `DASH_PROFILE_MODE=xam` ; (c) l'import copie xam.xex à la racine des données
   (`--shrdres-xam`) pour la génération des shaders HUD (33 générés, 1 non traduit : 817F9550_VS). Vérifié en conteneur
   depuis un répertoire neutre : import complet, XAM chargé, boîtes système rendues (texte + bouton + atténuation).
   Reste : 72 shaders dash non traduits (connu, point 4 ci-dessous).
   *Complément au 2026-11-21 (import hôte, **corrigé**)* : premier import complet sur l'hôte SteamOS —
   échec à `xex_rekey.py` (`ModuleNotFoundError: cryptography`, module présent dans le conteneur de
   build mais pas sur SteamOS). `tools/xex_rekey.py` réécrit **sans dépendance obligatoire** : AES par
   `cryptography`, sinon libcrypto/ctypes, sinon pur Python ; les trois fournisseurs sont équivalents
   octet pour octet (vecteur FIPS-197 tour par tour, SHA-256 identique du xam.xex rekeyé, testé sur
   l'hôte Python 3.14). `XEX_REKEY_AES=` force un fournisseur. Seul `xex_rekey.py` était concerné :
   tous les autres outils de l'import n'utilisent que la bibliothèque standard (vérifié par grep des
   imports). AppImage -xam reconstruite et redéposée dans `/home/deck/XenonDash/`.
   *Import hôte de l'utilisateur (2026-11-21, après-midi) : import complet réussi, Dashboard lancé —
   deux manques identifiés et traités :*
   - **Polices absentes** (texte invisible) : les paquets de mise à jour n'en contiennent pas (elles
     vivent dans la flash console). Les `.xtt` validés en conteneur venaient de `--modules`
     (dump xeBuild) ; sur l'hôte, la copie 6770 de repli du dépôt n'existe pas dans l'AppImage.
     Corrigé sur la machine : XenonCLatin/JKLatin/SCLatin.xtt (le jeu « Segoe UI (Kinect, Metro) »
     de l'utilisateur, tailles identiques au dump validé) copiés dans `<données>/flash/` et dans
     `/home/deck/XenonDash/polices/` (détecté par l'AppRun pour les imports suivants) ;
     `FONT_NAMES` ajoute SegoeXbox-Light.xtt (police XAM, extraite du su de la 17559, posée dans
     la flash pour le localisateur `XamGetLanguageTypeface`).
   - **`ERREUR SPIR-V introuvable VS 2F1495350021EBE2` au boot** : ce shader est `9201AA08_VS`
     (clé = XXH3 du microcode, retrouvée en extrayant les 258 conteneurs de dash.xex). Cause :
     le champ const d'une fetch fait 5 bits et ces VS encodent 19 alors que le matériel n'expose
     que 16 échantillonneurs — l'émission référençait `s19_*` sans déclaration. Correctif
     `patches/xenosrecomp/0005` (masque `constIndex & 15`, cohérent avec les lectures
     `g_TexBiasBits/g_TexExpScale` déjà masquées). Dans la foulée, `0006` : `SampleLevel` en VS
     (le LOD implicite est illégal en VS ; `XENOS_SAMPLE` + `XENOS_VERTEX_SHADER` émis avant le
     collage de shader_common.h) et déduplication de la signature d'entrée (le header des VS 13604
     répète les éléments par fetch constante). Résultat : dash 188/258 traduits (au lieu de 186),
     `2F1495350021EBE2.spv` généré dans les données, régénéré sur la machine de l'utilisateur.
     Restent : 6 VS en échec dxc (`iTexCoord6` dédoublonné mais mélange localisé/non localisé —
     demande l'extension d'ABI `USAGE_LOCATIONS` pour TexCoord8+, côté hôte `renderer.cpp` aussi,
     ce sont des maillages skinnés BlendWeight/Indices) et 64 VS où XenosRecomp **crashe**
     (returncode -11 = SIGSEGV, bloc contigu 920D6770→920F4AE8) — à investiguer sous gdb.
   *Shaders : **tous traduits** (2026-11-21, soir).* Sous gdb, les 64 SIGSEGV se réduisent à
   trois familles de défauts du header des VS 13604, traitées par `patches/xenosrecomp/0007`
   et `0008` :
   - `USAGE_LOCATIONS` sans TexCoord8+ (dxc refuse une signature mélangeant localisé/non
     localisé) → locations 16-19 pour TexCoord 8-11, **côté hôte aussi** (`ngpu_core/src/renderer.cpp`,
     `UsageLocation`) — les 6 VS skinnés passent ;
   - carte des interpolateurs VS indexée par position au lieu d'`interpolator.reg` (listes
     compactées du header 13604) + exports vers des interpolateurs non déclarés et fetches
     constantes non déclarées qui déréférençaient `end()` (assert muet en Release) → clé par
     registre, exports/fetches inconnus ignorés proprement (l'instruction est sautée, bloc
     prédiqué refermé) ;
   - textures 1D échantillonnées comme des 2D de hauteur 1 (`inResiduals`), les descripteurs
     partagés ne couvrant que 2D/3D/Cube.
   Résultat : **258/258 conteneurs dash et 34/34 conteneurs HUD traduits** (était 186/258 et
   33/34) ; le HUD `817F9550_VS` (même famille SIGSEGV) passe aussi.
   *Guide mort sur l'AppImage (2026-11-21, soir) : **corrigé et validé**.* Deux couches :
   (1) l'import **sautait** xam.xex lors de la copie des modules de la mise à jour —
   « module xam.xex recompilé : ÉCHEC » à chaque boot, aucune redirection XAM, G ne faisait
   rien ; `port_dashboard.py` copie désormais le xam.xex **rekeyé** (clé XEX1 → retail) dans
   la flash. (2) Ça ne suffisait pas : **strace** (l'AppImage refuse FUSE dans une session
   stracée, utiliser `--appimage-extract-and-run`) montre que le chargeur ouvre
   `<racine>/xam.xex`, pas `flash/xam.xex` — le montage `game:` du noyau (répertoire de
   l'exécutable) masque celui de la flash (préfixes insensibles à la casse, ordre de map
   arbitre, cf. commentaire de `MountFlash`) ; or la racine contenait le xam.xex **brut**
   (copié pour les shaders HUD, clé de session chiffrée XEX1) → « XEX load failed with code 2 »
   (hash de bloc, clé de session fausse), retail **et** devkit. Le code 2 n'est PAS un fichier
   absent : réplique Python de l'algorithme du loader (`/tmp/xdcheck.py`, jetable) sur le
   fichier rekeyé = hash du premier bloc MATCH. Correctif durable : `xenondash_import.py` pose
   le rekeyé **aux deux emplacements** (flash ET racine ; le repli `--shrdres-xam` rekeye
   aussi avant de copier). Validé sur la machine : « module xam.xex recompilé : **chargé** »,
   redirections `XamShowGuideUI (ordinal 772) -> XAM recompilé`, et la passerelle manette de
   XAM vit (`[xinputd] manette virtuelle 0x10000001 annoncée à XAM, port=0`,
   `XInputdGetCapabilities` polles en boucle) — la manette « morte depuis quelques sessions »
   avait la même cause (sans xam.xex, pas de pile d'entrée XAM).
   *Une seule AppImage (demande utilisateur, 2026-11-21) : `XenonDash-Kinect.AppImage`* —
   fin des noms versionnés/variantes (`XenonDash-2.0.13604.0[-xam]-x86_64.AppImage` abandonnés) ;
   l'argument de `make_appimage.sh` ne choisit plus que l'arbre de build, le nom de sortie est
   fixe, l'entrée .desktop devient « XenonDash Kinect ». Le répertoire de données reste
   `~/.local/share/xenondash/2.0.13604.0-xam/` (profils/flash/shaders conservés).
   *Disque simulé supprimé (demande utilisateur, 2026-11-21, 68c0aa8 + SDK d62f5dd).* Ce n'était
   pas un fichier : le cvar `dash_auto_disc` (vrai par défaut) présentait « le dernier jeu lancé
   de la bibliothèque » (ici Crackdown, seul jeu scanné) comme disque inséré — d'où
   « disque simulé : 4D5307DC dans le lecteur » au boot. Supprimé : le lecteur reste vide
   (directive), un jeu n'y va que sur `disc = true`/`--dash_disc`. `XamLoaderGetDvdTrayState`
   passe à 2 (fermé SANS disque ; XAM ne lit pas cet export, vérifié dans le code généré).
   *LA BOÎTE MODALE DU BOOT : ÉLIMINÉE (d0a352e).* Traque par instrumentation en chaîne dans le
   généré XAM ([msgdispatch] sub_8187B320 → [msgsend] sub_8187B4F0 → [appsend] sub_8187B8F0 →
   [appsend2] sub_8187BA98 → [notify254] sub_818C1188) : XAM n'envoie le message 0x00020023 à
   l'app 254 (qui ouvre la boîte, contenu variable « Game Error »/« Unrecognized Disc ») que si
   le **bit 3 (0x8) de XeKeysGetStatus est clair**. Le hook du projet
   (`xam_kernel_stubs.cpp`) ne remplissait jamais la sortie (r3 = pointeur de status, laissé à
   0). Correctif : `*r3 = 0xC` (bits 2|3 — le bit 2 est lu par deux autres sites ; sur console
   retail ces contrôles sont au vert). **Validé au banc : boot propre au premier essai**
   (boxroot=0), hub NXE direct sans aucune boîte (captures). Navigation au clavier validée au
   banc (les touches pad VK font défiler le carrousel).
   *Suppression COMPLÈTE de la fonction disque (d0a352e, suite de la demande)* : plus de
   `disc = true` dans games.toml (clé ignorée pour compat), plus de `--dash_disc`, plus de XEX
   synthétique sur `\Device\CdRom0` (dash_disc.cpp réécrit : icônes de bibliothèque + notification
   « aucun média » seulement). `XamLoaderGetMediaInfo/Ex` → « aucun média », `Resolve` sans
   repli « seul jeu déclaré » (les lancements passent par LaunchGame ou un chemin nommant un jeu).
   *Restant (front actuel)* : la scène du Guide **0x21015 renvoie E_FAIL** — le chargeur de
   l'app 254 (sub_8189BE20, switch sur scene-0x21001, cases 0..53) saute son case 20 à
   l'échec : le Guide s'ouvre (portier→1) mais sans contenu (spinner ou rien selon le flux).
   Qui doit charger 0x21015 (Guide : app 252 ? HUD ?) — chantier suivant. La manette réelle
   est à re-tester sur la machine (la chaîne pad→SDL→InputSystem→XInputd→XAM est vivante et
   la boîte qui bloquait n'existe plus).
   *État au 2026-11-20 (soir), **résolu*** : la variante `-xam` **affiche**. La cause du noir n'était
   pas dans le code invité mais dans le mappage mémoire : l'arène de 4,5 Go était un fichier
   `/dev/shm` mappé `MAP_SHARED`, et `/dev/shm` d'un conteneur fait souvent 64 Mo — au-delà,
   chaque écriture lève `SIGBUS` (1,6 M de fautes, écran noir, aucun message). Corrigé dans le SDK
   (`core/memory_posix.cpp`) par `memfd_create` avec repli `shm_open`, plus un message d'erreur
   explicite si le dimensionnement échoue. Mesure : 1 620 000 fautes -> **8**, 0 -> **6** `VdSwap`,
   image affichée (luma 123,7 contre 124,1 en référence HLE, soit la parité).
   **Sujet suivant, distinct** : ~~le HUD de XAM affiche une boîte modale `XamShowMessageBoxUI` qui ne
   se referme pas, et son fond atténué (`dim = 0,22`) assombrit toute l'image~~ **RÉSOLU (2026-11-21)** :
   le Guide et les boîtes système s'affichent avec leur contenu (lames, textes, boutons) et se ferment
   au bouton B. La cause du blocage était la diffusion hôte de `XN_SYS_UI` déclenchée par les pixels du
   chrome de chargement (elle re-posait le bit « interface active » de XAM juste après son boot) —
   diffusion passée en opt-in (`DASH_HUD_NOTIFY=1`). Autres correctifs de la même séance : le lien
   `GAME:` → `\Device\CdRom0` posé par XAM pendait (CdRom0 inexistant) et masquait `game:` pour les
   chargements de modules — CdRom0 monté vide (lecteur sans disque, directive utilisateur) et liens
   symboliques appariés sensiblement à la casse (SDK 0bc075f) ; chaînes localisées fonctionnelles
   (provider 0x81BFDA4C installé, textes « Unrecognized Disc », « Game Error »… rendus). Détail et
   mesures : `docs/xam_13604_compilation.md`, sections 2026-11-20/21.
   *État au 2026-11-16 (soir), mesuré* : la variante `-xam` **compile** (8 modules `.so`, `dash` lié),
   XAM **se charge** et son rendu natif **s'initialise** (`[ngpu] noyau ngpu_core sur llvmpipe`, sur
   **stderr** — jamais dans `dash.log`, ne pas s'y tromper). L'écran reste **noir** : le journal
   s'arrete apres ~3 s alors que le processus vit, et le thread principal attend des evenements X11
   sans qu'aucun thread invite ne lui demande de presenter. Detail, mesures et pistes dans
   `docs/xam_13604_compilation.md`.

   Quatre defauts reels ont ete corriges en chemin (tous mesures) :

   - **gestionnaire de faute du SDK** (`third_party/rexglue-sdk`, branche **`unified`**, commit
     `7348c20`) : `AccessViolationCallback` refusait tout heap autre que `kGuestPhysical`, alors que
     `SystemHeapAlloc` alloue dans `v00000000` (`kGuestVirtual`). Le premier ecrivain d'un module
     recompilé y provoquait un « Unhandled guest access violation » definitif. Violations : 1 -> 0.
   - **registre des modules** : les modules XUI sont charges sous `game:\flash\<nom>.xex`, normalise
     en `flash/<nom>.xex`. Le registre portait `"<nom>.xex"` : `FindRecompiledModule` echouait, donc
     aucune table de dispatch. Tables : 2 -> 4, plus aucun « function not in function table ».
   - **`KeSetTimerEx`** transmet desormais le DPC (`ctx.r6`) au lieu de passer 0 : la routine differee
     de XAM n'etait **jamais** appelee. Le message « DPC ignore » a disparu.
   - **chargement de XAM asynchrone** (`DASH_XAM_SYNC=1` pour l'ancien comportement) :
     `RunInSystemProcess` creait un thread puis faisait `Wait`, donc synchrone.

3. Avec le rendu natif (AppImage Kinect livrée) les fenêtres ImGui (progression de construction des jeux, liste F5, succès de remplacement) ne sont plus dessinées ; la recompilation d'un jeu n'a donc plus d'écran de progression visible. Les remplacer par des fenêtres du HUD de XAM (boîte de message avec progression, ou scène dédiée), ce que l'utilisateur veut de toute façon (pas de menus façon Xenia-canary).
   *Avancement (2026-11-17, palier N5)* : le **socle est commité** — `ShowXamMessageBox` appelable depuis l'hôte
   (`dash_xam_redirect.cpp`), thread invité unique + watcher de construction (`dash_xam_hud_ui.cpp`), redirection de
   `XamShowMessageBoxUI` dès que XAM est chargé, repli journalisé sans ImGui, tests `DASH_TEST_BUILDBOX=start|fail`.
   Vérifié : compilation des deux variantes, redirection résolue (ordinal 714 -> 0x8189A3C0), non-régression du hub
   13604 (capture regardée). **Les boîtes ne sont pas encore vues** : bloquées par l'écran noir de la variante `-xam`
   (point 2) ; toasts/mise à jour du texte et redirection des succès reportés au déblocage. Détail : `docs/xam_recompile.md`, palier N5.
   *Nuance (2026-11-15)* : ce n'est pas ce qui bloque l'utilisateur au quotidien. La navigation, les
   onglets et Game Library fonctionnent ; l'absence d'écran de progression ne se voit qu'au premier
   lancement d'un jeu à recompiler.
4. 72 shaders non traduits à l'import (64 plantages de XenosRecomp, 8 échecs `dxc`), 3 vertex shaders à texture ; l'un d'eux est créé à l'exécution.
5. Avatar : dépend d'un paquet de l'utilisateur (éditeur d'avatar absent du flash).
6. **Méthode** : chaque hypothèse doit être *mesurée avant* d'être annoncée, et une hypothèse
   réfutée doit être retirée du dossier, pas laissée « probable ». Sur cette session, quatre
   hypothèses successives ont été annoncées puis réfutées (attentes GPU sans borne, boucle de
   journalisation, gel déterministe, vtable nulle comme cause du gel) ; la vraie cause était une
   erreur de documentation. Un test A/B avec le code d'origine est le seul juge.
7. Recompilateur générique : XDK tardives (Trials Fusion refusé : image noire, shaders chargés hors `Set*Shader`).

## Volée du 2026-11-21 (soir) : freeze de Game Library corrigé, front Guide root-casé, manette instrumentée

Retour utilisateur sur l'AppImage 839476b1 : « manette sans réaction, G n'ouvre pas le Guide,
le Dashboard gèle si j'entre puis sors de Game Library ».

1. **Le « freeze » Game Library était un REX_FATAL** (processus avorté) : à la sortie de la
   bibliothèque, le destructeur de la tâche de chargement (`sub_922AA048`, appelé depuis la
   release `sub_922AA198` sur le thread de la tâche) appelle `vtbl[+4]` du sous-objet à `+36`
   de l'objet détruit — objet système (tas 0x40…, quatre handles noyau `F80003E8…F4` en +16..28)
   dont le sous-objet contient `2` au lieu d'une vtable : il n'a jamais été construit côté invité.
   Appel indirect vers 0 → `InvalidFunctionTrap` → abort. **Correctif : le trap n'est plus fatal**
   (SDK `function_dispatcher.cpp` : journal borné, `r3=0`, reprise après le `bctrl` ; `Execute()`
   gérait déjà l'absence sans fatal). Validé au banc avec les VRAIS jeux de l'utilisateur montés
   (`-v /home/deck/XenonDash/Games:/games:ro`, `XENONDASH_GAMES_DIR=/games`) : entrée + sortie de
   la bibliothèque sans FATAL, retour au hub propre (capture). Le `lr=0x922AA0D8` du journal
   utilisateur correspond exactement — même crash, même site.
2. **Piège gdb (nouveau)** : gdb intercepte SIGSEGV avant le récupérateur du SDK (`[rec2]`,
   restauration d'une page commitée) — un boot qui passe sans gdb plantait sous gdb (écriture à
   `base+0xFE451900` dans le remplissage d'image du hub). Pour déboguer le banc sous gdb :
   `handle SIGSEGV nostop noprint pass`. Le crash memset vu sous gdb n'existe donc pas hors gdb.
3. **`NicGetLinkState` n'écrit plus à travers r4** : l'appelant réseau `sub_81910F30` l'appelle avec
   le r4 HÉRITÉ du contexte (`sub_819231E8` ne charge que r3) ; écrire la page pointée a segfaulté
   quand elle n'était pas committée (gel du boot au banc). L'état « pas de lien » passe par r3=0 ;
   les appelants qui lisent `*r4` l'ont pré-rempli à zéro (même état).
4. **Front Guide root-casé (chaîne complète, sondes [guidetrace]/[guideio]/[scene]/[uistate])** :
   G → portier `sub_8189F0D0` accepte (1) → message `0x20004117` envoyé à l'app 254 (r3=0) →
   le chargeur de l'app 254 (`sub_8189BE20`, case 20) demande la scène `0x21015` → l'ouvreur
   `sub_818A21B0` EXIGE `UIstate(0x81BFCAE8)+172 != 0`, `+176 != 0`, `+216 == 0` — chez nous tout
   est à zéro → échec immédiat (E_FAIL) → rien ne se rend. Le semeur `sub_8189F050` (écrit +176,
   puis `KeSetEvent(+192)` si +172 déjà posé) **n'a jamais été appelé** (sonde d'entrée, zéro hit) ;
   il est déclenché par le message **`0x80000005` à l'app 254** (dispatch `loc_81895064` de
   `sub_81894D50`, payload avec u16 à +4 == 2) — message que personne n'émet dans notre
   environnement (trafic app 254 relevé : 0x00020005..0x0002000F, jamais 0x80000005). Sur console
   ce message vient vraisemblablement de la notification noyau « system UI » que l'HÔTE doit
   délivrer (listeners XNotify) à l'activation du shell. **Prochaine étape** : identifier l'émetteur
   attendu (ou armer l'état côté hôte au chargement de XAM) et trouver qui pose +172 (candidats :
   `sub_818A2C98`, `sub_8189F860`, `sub_8189E460`).
5. **Manette** : dans la session 12:56 de l'utilisateur, les boutons ARRIVAIENT au hook
   (`[xinputd] boutons 0x1000/0x2000/0x0400`) ; dans la session 14:20, AUCUN. SDL voyait pourtant
   les deux manettes (Steam Deck interne + Afterglow) et la passerelle xinputd était annoncée.
   Diagnostic ajouté : `XInputdReadState` journalise désormais « l'InputSystem n'a pas d'etat »
   (5 premiers échecs) — la prochaine session utilisateur dira si XAM ne pollé plus ou si SDL ne
   livre plus d'état (Steam Input ?). Le clavier, lui, marche toujours (hub navigable au banc).
6. Banc : une entrée de registre synthétique (games.toml sans fichiers réels) provoque un crash
   memset différent au premier present (remplissage de plans d'image UI, pointeur non commité) —
   n'existe pas avec de vrais jeux ; reproduire l'environnement utilisateur en montant son dossier
   de jeux, pas par une entrée synthétique.

