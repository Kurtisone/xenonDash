# Reprise du projet par une autre session ou un autre modèle

Ce document permet de reprendre le travail sans l'historique de conversation. Dernière mise à jour : 2026-11-14 (après le palier N4 : création de profil et connexion par XAM). Index de la documentation : [INDEX.md](INDEX.md) ; le README est destiné aux utilisateurs.

## Objectif en cours (NXE 2.0.13604)
Le Dashboard NXE doit rester dans l'interface Xbox : plus de fenêtres ImGui façon Xenia-canary ; sans profil, le même parcours qu'une console
(connexion, création de profil, clavier virtuel, avatar) avec les vraies fenêtres du HUD de XAM. **Rendu natif uniquement, zéro émulation**
(Xenia-canary seulement comme oracle de comparaison, `--gpu_plugin=xenos` idem). Blades (6770) et Metro (17559) : à reprendre quand l'utilisateur le demande.

## Où lire l'état
`git log --oneline | head -40` ; `PLAN.md` (P8) ; `docs/xam_recompile.md` (paliers A à N4 : XAM 13604, HUD, modules XUI, entrées, profil) ;
`docs/ngpu_core.md` (rendu natif du Dashboard et du HUD) ; `docs/nxe_jeux.md` ; `docs/recompilateur_generique.md` ; `docs/bibliotheque.md` ; `docs/technique.md`.
Notes détaillées des agents (non versionnées, `work/notes/`) : `xam13604-AVANCEMENT.md`, `hud_native-…`, `n3_modules-…`, `n4_profile-…` (si présent).

## Règles de l'utilisateur
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
- `pgrep -f` se retrouve lui-même dans une boucle d'attente `while pgrep -f ...` : toute attente doit avoir un délai maximal.
- Le fork du SDK est le sous-module `third_party/rexglue-sdk` (branche `unified`) ; après modification : le reconstruire et le réinstaller dans `sdk/` (procédure dans `docs/xam_recompile.md`), puis commiter le sous-module PUIS le dépôt.

## État du profil (palier N4, commité)
Création de profil, connexion et parcours « aucun profil » fonctionnent avec les vraies fenêtres Xbox (`DASH_PROFILE_MODE=xam`, défaut OFF ; variante `dashboards/2.0.13604.0-xam/`) :
pilote STFS minimal côté noyau, profil local hors ligne (XUID aléatoire, signature par une clé locale tirée au hasard), aucune clé de console. Détail : `docs/xam_recompile.md`.

## Suite, par ordre de priorité
1. Profil existant choisi dans la liste : XAM demande « Enter Pass Code » (cause non établie) ; `XamUserReadProfileSettings(Ex)` redirigé vers XAM plante le Dashboard (lecture invitée de `0x7008004F`) : gamertag/Gamerscore du bandeau et contenu restent liés au profil HLE ; fenêtre « Save Profile » absente du parcours Dashboard.
2. Rendre `DASH_PROFILE_MODE=xam` le défaut et livrer la variante `-xam` (XAM + HUD) en AppImage (aujourd'hui l'AppImage NXE est la variante sans XAM recompilé, donc sans HUD).
3. **Régression à corriger en priorité** : avec le rendu natif (AppImage NXE livrée) les fenêtres ImGui (progression de construction des jeux, liste F5, succès de remplacement) ne sont plus dessinées ; la recompilation d'un jeu n'a donc plus d'écran de progression visible. Les remplacer par des fenêtres du HUD de XAM (boîte de message avec progression, ou scène dédiée), ce que l'utilisateur veut de toute façon (pas de menus façon Xenia-canary). Vérifier aussi ce que le Dashboard affiche pendant la construction (écran figé ?).
4. 72 shaders non traduits à l'import (64 plantages de XenosRecomp, 8 échecs `dxc`), 3 vertex shaders à texture ; l'un d'eux est créé à l'exécution.
5. Avatar : dépend d'un paquet de l'utilisateur (éditeur d'avatar absent du flash).
6. Recompilateur générique : XDK tardives (Trials Fusion refusé : image noire, shaders chargés hors `Set*Shader`).
