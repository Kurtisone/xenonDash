# Plan : les Dashboards Xbox 360 en natif Linux

## Objectif

Une version par interface, la plus récente de chaque génération : **Blades** (2.0.6770), **Kinect** (2.0.13604) et **Metro** (2.0.17559), recompilées en natif (rendu D3D natif, XAM recompilé quand un `xam.xex` complet existe),
livrées en AppImage et capables de **lancer des jeux** recompilés à la demande. Les fichiers Microsoft ne sont jamais versionnés : l'AppImage les importe d'une mise à jour officielle fournie par l'utilisateur.
**Front actuel : le Kinect 13604** (HUD, profil, Guide, avatar, jeux). Blades et Metro sont reportés à la demande de l'utilisateur. Reprise du travail : [docs/REPRISE.md](docs/REPRISE.md).

## Décisions

| Sujet | Décision |
|---|---|
| Runtime | **ReXGlue en fork** (`third_party/xenondash-sdk`, branche `main` ; contient les anciennes `xenondash` et `unified`) : noyau HLE, audio, entrées. Le remplacer reviendrait à réécrire Xenia |
| XAM | **Recompilé** (module ReXGlue) quand un `xam.xex` complet et déchiffrable de la même version existe ; sinon HLE. Blades 6770 : base 1888 indéchiffrable, HLE |
| Versions | Une par interface ; 6717, 7258, 5759, 4532, 2255 restent en l'état |
| Zéro émulation | GPU par rendu natif D3D (`ngpu_core`, shaders traduits hors ligne) ; jeux recompilés seulement, un jeu sans rendu natif est refusé ; Xenia et le plugin `xenos` ne servent que d'oracle de comparaison |
| Licence | **GPL-3.0** (reprise du code de Skate1Native) ; accord de TekRantGaming pour le code de TrialsReVolution |
| Lecteur optique | simulation de disque supprimée (2026-11-21) : lecteur vide et muet ; les jeux passent par le dossier de jeux |
| Distribution | binaires recompilés = traduction de code Microsoft : usage personnel, jamais publiés |

## Phases

- **P0 Restructuration multi-versions (fait)** : `common/` partagé, `dashboards/<version>/`, SDK en sous-module du fork. [docs/technique.md](docs/technique.md).
- **P1 Contenu, Stored Game Items (fait)** : sauvegardes et téléchargements en paquets STFS par profil, énumération, métadonnées, suppression, copie, import (`tools/content_import.py`). Reste : voir To-Do.
- **P2 Chaîne de portage (fait)** : `tools/port_dashboard.sh` (extraction, assets, codegen en boucle, build, smoke) ; 6770 régénéré à l'identique.
- **P3 Versions Blades (en partie, reporté)** : 6717 OK ; 7258 partiel ; 5759 et 4532 écran noir ; 2255 impossible (deltas sans base). Tableau : [docs/technique.md](docs/technique.md).
- **P4 XAM recompilé (Kinect fait en grande partie)** : 17559 en HLE affiché, prototype de XAM recompilé ; 13604 : XAM, HUD et 8 modules XUI recompilés, redirection des imports, entrées, profil, Guide, avatar ([docs/xam_recompile.md](docs/xam_recompile.md)). Reste : voir To-Do. Metro reporté.
- **P5 Lancement des jeux (fait)** : registre `games.toml`, hooks `XamLoaderLaunchTitle*`, superviseur détaché qui relance le Dashboard au retour ; My Games, Play Game ([docs/technique.md](docs/technique.md), [docs/jeux.md](docs/jeux.md)).
- **P6 AppImage (fait)** : `XenonDash-Kinect.AppImage` (rendu natif, import au premier lancement, shaders générés, chaîne de recompilation embarquée). Reste : voir To-Do (tests sur le Deck).
- **P7 Rendu natif D3D (fait pour le Kinect)** : noyau `ngpu_core`, façades 6770, 13604 et HUD de XAM, parité mesurée contre l'oracle `xenos` ([docs/ngpu_core.md](docs/ngpu_core.md)). Reste : voir To-Do (32:9 reporté).
- **P8 Fusion des ports, recompilation à la demande (fait en grande partie)** : SDK et XenosRecomp unifiés, noyau commun, recettes (`games/<TitleID>/`, `tools/recipe_build.py`), bibliothèque de jeux, recompilateur générique
  ([docs/jeux.md](docs/jeux.md)). Skate de bout en bout depuis la Game Library (12 à 25 min à froid) ; Skate en mode générique atteint son écran-titre. Les fenêtres ImGui sont retirées : le rendu est natif, aucune fenêtre ImGui ;
  la progression de construction passe par une boîte de message de XAM (palier N5). Reste : voir To-Do.

## Risques connus

- Lavapipe (rendu logiciel sous Xvfb) se fige sur certaines transitions : les points que le banc ne vérifie pas se testent sur le Deck.
- Exceptions C++ du Dashboard fatales (`RtlUnwind` absent du SDK) ; stubs du SDK qui renvoient leur 1er argument ou n'écrivent pas leurs sorties : à auditer ([docs/REPRISE.md](docs/REPRISE.md), « Pièges »).
- Temps, disque et mémoire de la recompilation d'un jeu ; chaîne de compilation chez l'utilisateur.

## To-Do (Kinect 13604)
Par ordre de priorité. Ce que le banc ne peut pas vérifier se teste sur le Deck.
1. **Interface Xbox seulement (fait en grande partie)** : `XamShowAchievementsUIEx`, `XamShowGamerCardUI` et `XNotifyQueueUI` par XAM recompilé (liste par défaut) ; les autres imports d'interface restent des stubs du SDK, non appelés dans le parcours hors ligne ([docs/xam_recompile.md](docs/xam_recompile.md), section redirection). Progression de construction : boîte puis notification par étape (le texte d'une boîte ouverte est figé). Reste : relier le bouton « Achievements » du hub (`dash_guide_actions.cpp`), confirmer sur une vraie construction.
2. **Profil (fait en grande partie)** : sous-menus de Customize Profile (image, nom, avatar), connexion d'un profil sans avatar, création jusqu'à « Save Profile » et panneau du Guide après B validés au banc (2026-11-26). Reste : `XamUserReadProfileSettings(Ex)` et `Write` vers XAM (lecture par XAM : gamertag et avatar justes, image du joueur absente ; voir [docs/xam_recompile.md](docs/xam_recompile.md), problèmes ouverts) ; « Enter Pass Code » à confirmer sur le Deck.
3. **Avatar (fait en grande partie)** : Customize Avatar atteint `XamLaunchAvatarEditor` (mode d'expérience et index lus dans XAM pour cet appel), choix d'avatar par le MiniCreator (option 2) et avatar mis à jour dans My Xbox. Reste : le MiniCreator d'option 2 n'a pas de bouton B ; `AvatarEditor.xex` plus tard.
4. **Succès (à moitié fait)** : `XamShowAchievementsUIEx` et toasts par XAM validés au banc (panneau « Achievements » à 0 succès, toast bas centre). Reste : succès réellement débloqués par un titre (aucun profil de test n'en a), notifications `XN_` de déblocage.
5. **Manette du Deck** : boutons parfois absents (pad → SDL → InputSystem → XInputd) ; à trancher avec un `dash.log` du Deck.
6. **Bibliothèques média** : lecture d'un média ; sources Computer, Portable Device et Current Disc.
7. **Lecteur optique réel** exposé en `\Device\CdRom0` (sans vrai lecteur : vide et muet).
8. **Jeux** : XDK tardives (Trials Fusion : image noire) ; ISO et GOD réels ; plusieurs jeux en mode générique.
Reportés à la demande de l'utilisateur : étirement 32:9 ; Blades (6770) et Metro (17559).
