# Avatar du hub (13604)

Extrait de `REPRISE.md`. État au 2026-11-24 : l avatar choisi s affiche sur le XMB (banc).

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
- **Avatar affiché sur le XMB (2026-11-24, banc)** : la silhouette grise avait quatre causes successives,
  toutes corrigées :
  1. `XamAvatarGetAssets` renvoyait des tampons vides : les fonctions noyau `LDI*` (décompression LZX) étaient
     des stubs du SDK. Implémentées dans `common/src/xam_kernel_stubs.cpp` (`243b35e`).
  2. Le recompilateur du SDK n'avait pas `vavguw`, `crmove` ni `crnot` (plantage du code de XAM). Ajoutées
     (dépôt SDK imbriqué, `69764e99`).
  3. Les shaders de sommets de l'avatar lisent leurs matrices d'os par `vfetch` indexés (BLENDINDICES). XenosRecomp
     ignorait l'index : patch `0009-dash13604-indexed-vfetch.patch` et tampon bindless (`105b468`).
  4. Sous tuilage prédit, la hauteur de la texture résolue était `y + destHeight` au lieu de `destHeight`
     (`ngpu_core/src/renderer.cpp`, `687ecab`).
  Restent : `Customize Avatar` de dash.xex n'atteint pas `XamLaunchAvatarEditor` (`XamPackageManagerGetExperienceMode`
  du HLE sans le bit 0x2) ; l'AppImage avec ces correctifs sur le Deck, à partir d'un premier lancement avec le zip.
  La carte d'ombre de l'avatar est noire (passes de profondeur seule ignorées) mais sans effet visible : l'ombre
  au sol est un sprite fixe du XMB, qui ne suit pas l'avatar, et l'avatar n'est pas assombri (captures
  `dr3/11_trig.png`, `av4/10_wait.png`). Pas de correctif ; à revoir si l'occlusion pose problème ailleurs.
- **Régression corrigée puis validée** : après connexion, le Guide affichait un panneau vide (XAM chargeait
  l'XEX d'avatar non recompilé). Corrigé par le refus de `XexLoadImage` ci-dessus ; le Guide revient au menu.
  Reste : après connexion, le Guide se ferme au lieu de rester ouvert (à traiter : faire échouer
  `XamShowAvatarMiniCreatorUI` dans `dash_xam_redirect.cpp`, maintenant que le module est recompilé).
