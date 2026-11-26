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
- **Point d'arrêt matériel en écriture, dans le processus** (qui a écrit cette valeur ?) : `perf_event_open` avec
  `PERF_TYPE_BREAKPOINT`, `HW_BREAKPOINT_W`, `bp_addr` = adresse HÔTE (base invitée + adresse), `sigtrap = 1`,
  `remove_on_exec = 1`, `inherit = 1`, `inherit_thread = 1`, ouvert sur CHAQUE fil de `/proc/self/task` ; le gestionnaire de
  SIGTRAP ne traite que `si_code == TRAP_PERF` (`backtrace_symbols_fd` : les `sub_…` du code généré sont des symboles
  dynamiques) et chaîne les autres SIGTRAP au gestionnaire précédent. Marche avec `ptrace_scope = 1` et
  `perf_event_paranoid = 2`, sous le banc, sans gdb ; pile dans `out.log`. Code temporaire dans
  `dashboards/<variante>/src/` (pris par GLOB), à retirer ensuite. A trouvé en une exécution l'écriture de
  `XamLoaderGetDvdTrayState` (prototype faux du SDK) dans une vtable du Dashboard (2026-11-26).
- **Ghidra** : `tools/ghidra/xd_ghidra.py decompile xam <fonction>` donne le C décompilé d'une fonction de XAM ou de
  dash.xex avec les noms partagés de `symbols/2.0.13604.0/` (voir [ghidra.md](ghidra.md)) ; y consigner tout nom établi.
  `tools/function_inventory.py` affiche les mêmes noms.
- **Chaque hypothèse doit être mesurée avant d'être annoncée, et une hypothèse réfutée doit être
  retirée du dossier** (des sessions entières ont été perdues sur des hypothèses non réfutées à temps).
- Vérifier chaque résultat visuel par **capture regardée** (Xvfb + lavapipe), jamais par le seul
  journal. Écrans X dédiés par tâche (:92, :94, …) ; `ulimit -c 0` avant dash ; `XKEYS_HOLD=0.12`.
- `tools/banc.py` : banc pas à pas (`start`, `keys`, `shot`, `wait`, `cpu`, `grep`, `stop`) qui lance le Dashboard Kinect
  (`dashboards/2.0.13604.0-xam`, seule variante 13604 depuis le 2026-11-26 : XAM recompilé toujours actif) ou un autre
  (`--build`, et `--unset DASH_XAM_MODULE --unset DASH_PROFILE_MODE` pour 6770 ou 17559, sans XAM recompilé) comme
  l'AppImage, sur une COPIE du dossier de données (profils de l'utilisateur intacts) et une COPIE de la
  bibliothèque de jeux (`XENONDASH_LIBRARY_DIR` : sans elle, le scan du démarrage réécrit
  `~/.local/share/xenondash/library` avec le dossier de jeux du banc), écran dédié sans pointeur de souris (`Xvfb
  -nocursor` : sinon il apparaît ou non au centre des captures selon l'instant), arrêt par PID.
  `cpu` donne la charge par fil : écarter les fils `llvmpipe-*` (rendu logiciel de lavapipe).
  Plantage sans trace (pas de core gardé) : `start --gdb` lance dash sous gdb et écrit dans `out.log` la pile du fil
  fautif, l'instruction et les registres de la faute ; `--gdb-pre F` lit des commandes gdb avant le lancement (points
  d'arrêt, surveillance matérielle en Python), `--gdb-x F` après la pile.
- `tools/banc_suite.py run` : suite de non-régression sur ce banc, verdict OK/KO (code de sortie 0/1), ~4 min. Un
  parcours fixe (Welcome, My Xbox, Game Library aller-retour, Video Library (écran Select Source), Console Settings, Screen
  Resolution, Guide, Sign In, Guide connecté) : captures comparées aux références, journal (aucun `*** STOP` ni `[fibre]`, découverte de l'affichage
  présente, aucune ligne `[error]` hors de la liste `ERROR_ALLOWED`, vide depuis le 2026-11-26 : une erreur est un défaut à
  corriger ou un cas normal à journaliser plus bas) et charge par fil une fois le Guide fermé. Le rendu lavapipe est presque déterministe (écart nul entre deux
  exécutions une fois masqués l'horloge du Guide et le logo Xbox animé) : les tolérances sont serrées et un texte changé
  fait échouer le point. Vérifié en provoquant les deux régressions connues : `--env DASH_VIDEO_DISCOVERY=0` (KO
  Console Settings et Screen Resolution, découverte absente du journal), `--env DASH_XAM_FIBER_FIX=0` (KO charge, un
  fil à 100 %).
  Références, tolérances, instantané des données, liens vers les jeux et bibliothèque scannée hors du dépôt :
  `<copie principale>/work/banc/refs/2.0.13604.0-xam`. Après un changement VOULU de l'interface : deux `run`, regarder
  les captures, puis `refs <exécution> --calib <autre exécution>` ; `init` refait l'instantané (données, jeux,
  bibliothèque).
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
- **Journal de la connexion** : `DASH_LOGON_TRACE=1` suit la tâche de connexion de XAM (`sub_818B2BF8`, XamUserLogonEx) et ses
  étapes (`src_xam_patch/xam_hud_trace.cpp`) ; chaque `XamUserLogon(Ex)` du Dashboard est journalisé par défaut avec ses XUID
  et son appelant (`[xamredir]`, XUID tous nuls = déconnexion de tous). `DASH_HUD_RESOLVES=1` : chaque forme de Resolve du HUD
  une fois (destination, pas, rectangle), pour voir quelles surfaces XAM compose.
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
- Le fork du SDK est le sous-module `third_party/xenondash-sdk`, branche `main` (depuis le 2026-11-26, elle contient les
  anciennes `dash-13604-recompilateur`, `unified` et `xenondash`) ; après modification : reconstruire, réinstaller dans `sdk/`, commiter le sous-module PUIS
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
  chercher d'abord les `STUB` du journal sur le chemin. Même piège sans argument : le stub laisse dans r3 la valeur de
  l'appelant (souvent `this`, inférieur à 0x80000000), lue comme un HRESULT de succès (`XamIsIptvEnabled` : emplacement
  Mediaroom vide dans My Xbox, 2026-11-26). Un prototype FAUX est pire qu'un stub : `XamLoaderGetDvdTrayState` (sans
  argument) écrivait à travers r3 dans l'objet de l'appelant (appel indirect nul plus tard, loin de la cause).
- **Fuseau horaire** : XConfig 3/1..7 vient du fuseau de l'hôte (`TZ` ou `/etc/localtime`, `common/src/host_timezone.h`),
  rapproché de la table des fuseaux du Dashboard (mêmes biais et dates, nom de l'hôte préféré). La table est cherchée dans
  l'image chargée du Dashboard (`common/src/timezone_table.h`), sans adresse par version : 2.0.4532 à 2.0.13604, entrées de
  32 octets à noms par pointeur (13604 : 0x929531A8, 77 entrées) ; 2.0.17559, entrées de 244 octets avec une
  TIME_ZONE_INFORMATION de Windows (0x9295C440, 83 entrées). Un nom absent de la table (« CET ») faisait réécrire GMT par le
  Dashboard au démarrage : horloge du Guide en UTC. Sans entrée de mêmes règles, les noms sont rendus vides et le Dashboard
  ne réécrit rien. Vu à l'écran (variante -xam, hôte Europe/Paris) : Console Settings > Clock et la liste Time Zone donnent
  « GMT+01 Berlin », qui a les mêmes règles que « GMT+01 Paris, Madrid » (la chaîne POSIX de l'hôte, CET/CEST, ne les
  distingue pas : la première entrée de ces règles l'emporte). Tests : `common/tests/host_timezone_test.cpp`,
  `common/tests/timezone_table_test.cpp` (tables synthétiques ; avec des images écrites par `tools/xexinfo`, la table de
  chaque Dashboard et l'entrée retenue pour l'hôte). 6770 (Blades) : Clock donne aussi « GMT+01 Berlin » ; ses fuseaux
  américains ont les règles d'avant 2007, un hôte de New York n'y trouve donc pas d'entrée (noms vides, heure juste).
  **17559 (Metro) : noms laissés vides** (`kernel_xconfig.cpp`). Avec les noms de l'entrée retenue, Metro plante dès que
  la sélection passe sur Clock : son fil d'entrée 0x92392848, lancé au démarrage, se termine avec un X_KTHREAD (0x30057018)
  entièrement à zéro, et `XThread::RundownAPCs` lit une liste d'APC à flink nul (pile obtenue par `banc.py start --gdb`,
  2026-11-26). Avec des noms vides, pas de plantage. L'auteur de la remise à zéro n'est pas trouvé : une surveillance
  matérielle de l'en-tête (`--gdb-pre`) n'a vu aucune écriture jusqu'au menu, ce qui oriente vers une remise à zéro sans
  écriture visible (libération puis réallocation de la page, `BaseHeap::Release`/`Decommit`) ; à reprendre avec Metro.
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
5. **Bibliothèques média** : Video, Music et Picture Library ouvrent leur écran Select Source, et Downloaded Videos sa
   bibliothèque (vide), depuis que les messages XMP du Dashboard vont à XAM recompilé (2026-11-26, voir
   [xam_etat.md](xam_etat.md) ; point `videolib` de la suite). Le retour au hub venait du message XMP 0x0007002B, inconnu
   du HLE du SDK, pas du gestionnaire de paquets : `XamPackageManagerFindPackageContainingIndexedXEX` (stub) n'est appelé
   qu'une fois au démarrage, avant comme après la correction. Reste à voir : lecture d'un média (le banc n'en a aucun),
   sources Computer (partage réseau), Portable Device et Current Disc (grisées sans périphérique).
6. **Divers** : glyphe Gamerscore `U+E00A` (corrigé par `dash_font_patch.cpp`, voir `technique.md`) ; recompilateur générique pour XDK tardives
   (Trials Fusion : image noire, shaders hors `Set*Shader`) ; vrai lecteur optique exposé comme
   `\Device\CdRom0` (directive utilisateur) ; étirement 32:9 de l'AppImage (reporté à la demande de l'utilisateur).
