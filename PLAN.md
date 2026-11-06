# Plan : tous les Dashboards Xbox 360 en natif Linux, un AppImage par version

## Objectif

Chaque version du Dashboard (2.0.2255 → 2.0.17559) recompilée en natif, livrée sous forme d'**un
AppImage par version**, capable de **lancer les jeux**. Les fichiers Microsoft (firmware, XEX, polices)
ne sont jamais versionnés : l'AppImage les importe depuis une mise à jour officielle fournie par
l'utilisateur au premier lancement.

## Décisions

| Sujet | Décision | Raison |
|---|---|---|
| Runtime | **Garder ReXGlue, en fork** (`third_party/rexglue-sdk` → dépôt Forgejo `rexglue-sdk`, branche `xenondash`) | Il fournit noyau HLE + GPU Xenos/Vulkan + audio + entrées. Le remplacer = réécrire Xenia. Nos 3 correctifs sont des commits du fork (**fait** : sous-module git, branche `xenondash`). Réévaluer seulement si un blocage du SDK devient insoluble. |
| XAM | **Recompilé** (module DLL ReXGlue) quand un `xam.xex` complet et déchiffrable de la **même version** existe ; sinon **émulation haut niveau** (HLE ReXGlue + `dash_*.cpp`) | Mélanger Dashboard et XAM de versions différentes n'a jamais existé sur console. |
| Versions Blades (≤ 7258) | HLE pour l'instant (XAM 6770 = base 1888 indéchiffrable + delta) ; recompilation de XAM si une base déchiffrable est obtenue | — |
| Versions NXE/Metro avec XAM complet (17559, autres mises à jour complètes) | Dashboard **et** XAM recompilés | Écrans XAM d'origine (succès, Guide…) au lieu des remplaçants ImGui. |
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

### P2 — Chaîne de portage automatisée
- `tools/port_dashboard.sh <mise_à_jour>` : extraction STFS, déchiffrement, `rexglue codegen`,
  boucle de corrections automatiques (cibles non résolues, fonctions par pointeur, continuations de
  `catch` exclues), compilation, test de fumée sans écran (capture + absence de FATAL).
- Critère : 6770 régénéré entièrement par l'outil, résultat identique.

### P3 — Portage des versions Blades
- 2.0.7258, 6717, 5759, 4532, 2255 via P2 ; corrections manuelles documentées par version.
- Flash par version : modules et polices (XeBuilds), `shrdres` converti.

### P4 — Versions avec XAM recompilé (17559 d'abord)
- Dashboard 17559 + XAM 17559 recompilés ; XAM comme module ReXGlue (`[[modules]]`), ses exports
  remplaçant ceux du HLE ; démarrage de XAM (threads système, notifications, Guide).
- Les remplaçants ImGui (`dash_xam_ui.cpp`) ne servent plus pour ces versions.
- Ensuite : autres mises à jour complètes NXE/Metro.

### P5 — Lancement des jeux
- `XamLoaderLaunchTitle` / `XamLoaderLaunchTitleEx` : registre `games.toml` (TitleID → commande du
  port natif, ou chemin XEX/ISO pour Xenia) ; Games Library alimentée par ce registre.
- Retour au Dashboard (relance) à la fin du jeu ; mise à jour des jeux joués/succès si le port les
  écrit (format GPD).
- Critère : Skate (port natif Skate1Recompiled) lancé depuis le Dashboard puis retour.

### P6 — AppImage
- Un AppImage par version (`appimagetool`/`linuxdeploy`) : binaire, `librexruntime.so`,
  `librexgpu-xenos.so`, dépendances SDL/Vulkan ; premier lancement : import de la mise à jour
  officielle (`$SystemUpdate` ou archive) et conversion des ressources dans
  `~/.local/share/xenondash/<version>/`.
- Critère : AppImage 6770 lancé sur un bureau sans rien d'autre installé.

## Risques connus
- Rendu logiciel (lavapipe/Xvfb) qui se fige sur certaines transitions : tests sur vrai GPU requis.
- Exceptions C++ du Dashboard fatales (`RtlUnwind` non implémenté dans le SDK) : à implémenter dans le
  fork si elles se multiplient.
- Stubs du SDK qui renvoient leur premier argument : à auditer (fonctions booléennes `XamIs*` surtout).
- Le binaire recompilé contient une traduction du code Microsoft : AppImages pour usage personnel,
  non redistribuables.
