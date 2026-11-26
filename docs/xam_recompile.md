# XAM, HUD et modules XUI 13604 recompilés

Dashboard Kinect 2.0.13604 (`dashboards/2.0.13604.0-xam/`, seule variante) : `xam.xex`, `hud.xex` et les modules XUI de la flash sont
recompilés par ReXGlue et chargés dans le processus du Dashboard. Le rendu est natif (aucun plugin GPU, aucune émulation). Ce
document remplace `xam_13604_compilation.md`, `xam_etat.md` et `avatar_hub.md` : les journaux datés et les pistes réfutées sont dans
git (liste en fin de fichier). État courant : `docs/REPRISE.md`. Les mesures datent de novembre 2026, au banc (Xvfb + lavapipe).

## 1. Architecture

### Modules recompilés (`dash_manifest.toml`, entrées `[[modules]]`)

| Module | Chemin invité | Base fixe | `function_table_base` | Rôle |
|---|---|---|---|---|
| `xam` | `game:\xam.xex` | `0x817E0000` (taille `0x570000`) | `0x84000000` | XAM : applications système, profils, UI, noyau de session |
| `hud` | `game:\flash\hud.xex` | `0x913E0000` | `0x84800000` | HUD : boîtes, Guide, scène XUI (1133 fonctions) |
| `vk` | `game:\flash\vk.xex` | `0x917D0000` | `0x84900000` | clavier virtuel |
| `signin` | `game:\flash\signin.xex` | `0x90100000` | `0x84A00000` | connexion / « No Profiles Found » |
| `createprofile` | `game:\flash\createprofile.xex` | `0x90100000` | `0x84B00000` | « Enter Profile Name » |
| `gamerprofile` | `game:\flash\gamerprofile.xex` | `0x90100000` | `0x84C00000` | carte de joueur |
| `deviceselector` | `game:\flash\deviceselector.xex` | `0x916D0000` | `0x84D00000` | sélecteur de périphérique (compilé, non essayé) |
| `ximecore` | `game:\flash\ximecore.xex` | `0x91680000` | `0x84E00000` | éditeur de méthode de saisie (compilé, non essayé) |
| `Guide_AvatarMiniCreator` | `\sep\20352400\Guide.AvatarMiniCreator.xex` | `0x90100000` | `0x84F00000` | choix d'avatar |

`huduiskin.xex` n'est pas du code : archive de ressources (`XamStrings.xus`), chargée par le SDK comme « module de ressources ».
`AvatarEditor.xex` (éditeur complet, 15 Mo) n'est pas recompilé.

### Bases fixes et tables de fonctions
- Les XEX ne sont pas relocalisables (la « `.reloc` » décodée n'est pas une table). `signin`, `createprofile`, `gamerprofile` et
  `Guide_AvatarMiniCreator` partagent `0x90100000` : un seul est chargé à la fois, à la demande (sur console aussi).
- Chaque table de fonctions est placée avec `function_table_base` (clé TOML de `[[modules]]`, ajoutée au codegen du SDK ; sans la clé :
  fin de l'image). Par défaut la table de XAM tombait en `0x81D50000`, soit `0x82000000-0x82477098` : le début de dash.xex (les tas
  `0x8` et `0x9` étaient deux vues du même fichier). Elle détruisait l'enregistrement d'import `0x92000D64` (FATAL de `DASH_XAM_MODULE=1`).
- Le SDK sépare désormais les tas XEX `0x8` et `0x9` par défaut (`REX_ALIAS_XEX_HEAPS=1` rétablit l'alias). `vk` (`0x917D0000`)
  recouvrait sinon l'image de XAM : le codegen lisait des tables de saut à zéro (SIGILL « switch case out of range »).
- `UnloadUserModule` du SDK retrouve le module par une table de clés (`recompiled_module_keys_`) : le rechargement d'un XUI marche.
- Plages physiques : dash.xex `0x82000000-0x82EA8000`, XAM `0x817E0000-0x81D50000`, HUD `0x813E0000-0x8142C000` ; tables sans recouvrement.

### Régénération
- `python3 tools/regen_xam_modules.py` (SDK installé dans `sdk/`, environ 2 min) : codegen par groupes `--target` (le codegen charge tous
  les XEX avant d'analyser) : {xam, hud, vk, deviceselector, ximecore, signin}, puis createprofile, gamerprofile,
  Guide_AvatarMiniCreator ; chemin ABSOLU du manifeste (sinon l'empreinte diffère et CMake régénère tout) ; puis
  `tools/xam_unimpl_patch.py` (crmove `0x81936D14`, crnot `0x81936D28`, vavguw `0x81B19A48` ; le SDK 69764e99 les génère maintenant) et
  `tools/gen_module_xam_imports.py` (fichiers `src_hud/`, `src_xui/`). À relancer après tout changement de manifeste ou d'overrides.
- Les overrides par module (`<nom>_overrides.toml`, `<nom>_auto_overrides.toml`) viennent de `tools/codegen_module.py` ; XAM : 16 914
  fonctions, 9 cibles non résolues, 83 fonctions par pointeur. Le code généré (`generated/`) n'est pas versionné ; les sondes qu'on y
  met sont perdues à la régénération.
- Un `rexglue codegen --target` isolé écrase `dll_targets.cmake` et `module_registry.cpp` : passer par le script. Le registre porte
  des chemins normalisés (`flash/<nom>.xex`). `CMakeLists.txt` ajoute `generated/<dir>/dash_register.cpp` (le gabarit du SDK l'a retiré
  en `e28335b`) et `ENABLE_EXPORTS ON` sur `dash` (la DLL appelle `xam_skip_device_auth`, `hudd3d_call`, `CallModuleImport`).
- Fichiers (Microsoft, ignorés par git) : `assets/flash/xam.xex` et lien `assets/xam.xex` (`game:\xam.xex` est lu à la racine ; `assets`
  est un lien de répertoire). `xam.xex` est chiffré avec la clé XEX1 : `tools/xex_rekey.py` (AES : `cryptography`, sinon libcrypto, sinon
  Python pur ; `XEX_REKEY_AES=` force) le rechiffre en retail ; la version se lit par `tools/xexinfo/build/xexinfo` (`xboxkrnl v2.0.13604`).
  Source correcte : `xeBuild-Data-13604.zip`. Le rechiffré doit être aux DEUX emplacements (flash ET racine des données) : code de
  chargement 2 = hash de bloc invalide (mauvaise clé), pas fichier absent.

### Chargement (`dash_app_base.cpp`, `dash_xam_runtime.{h,cpp}`)
- `main.cpp` pose par défaut `DASH_XAM_MODULE=2` (module chargé et point d'entrée exécuté ; `1` = chargé sans point d'entrée) et
  `DASH_PROFILE_MODE=xam` ; une valeur déjà posée l'emporte. Ordre : flash montée (`MountFlash`) avant XAM ; `Partition1`, `MU0`
  et `\Device\CdRom0` (dossier vide) sur `DASH_XAM_STATE` (défaut `${XDG_DATA_HOME:-~/.local/share}/xenondash/xam`) ; `game:` sur la flash.
- XAM se charge sur un thread système asynchrone (`RunInSystemProcess`, `XThread` du processus système ; `DASH_XAM_SYNC=1` : synchrone).
  `KeGetCurrentProcessType()` rend 2 si `ctx.lr` est dans l'image de XAM ou des modules HUD (`InXamImage`, `0x90000000-0x91800000`).
- Hook `XexLoadImage` : un nom nu demandé par XAM (`hud.xex`, `vk.xex`…) devient `game:\flash\<nom>` ; le module `Guide.AvatarMiniCreator`
  est reconnu sous `\sep` et sous `\Device\Harddisk0\SystemExtPartition`, les autres XEX de cette partition sont refusés.
- Modules de ressources (`huduiskin.xex`) : SDK `XexModule::is_resource_module()` ; `XexGetModuleSection` lit les ressources nommées.
- XAM charge et décharge les XUI à la demande : `sub_8189BE20` (application 254) copie le nom dans l'état du HUD (`0x81BFCAE8 + 224`),
  `sub_81894210` fait `XexLoadImage(nom, 0x40000008)`, `sub_818943E8` décharge ; le module termine par `XamAppUnloadSelf`.
- `[[midasm_hook]]` de `xam_overrides.toml` : `sub_818CD070` (écoute `\Device\DeviceAuth`, sinon `KeBugCheck(0x29)`) est neutralisée
  par `xam_skip_device_auth` (`src/xam_neutralize.cpp`). Applications de XAM : table statique `0x817E4B60` (0xFC xlivebase, 0xFD uvbase,
  0xFB xgi, 0xFA musicplayer, 0xF7 LiveMessenger, 0xF5 ximecore, 0xF4 Community, 0xF2/0xF3 avatar, 0xF8 cert), enregistrées par `sub_818951C8`.
- `KeBugCheck`/`KeBugCheckEx` journalisent (`[kernel] KeBugCheck… lr=`), puis arrêt fatal (SDK `7f60045f`) ; le recompilateur termine le bloc après le `bl`.

### Redirection des imports du Dashboard (`common/src/dash_xam_redirect.{h,cpp}`)
- Un hook `REX_HOOK_RAW(__imp__<Nom>)` par import `xam.xex` du Dashboard (macro `DASH_XAM_REDIRECT_HOOK`). `Dispatch` exécute l'export de
  XAM recompilé de même ordinal (`GetProcAddressByOrdinal`, puis `FunctionDispatcher::GetFunction`) avec le même `ctx`, sinon le HLE du SDK.
  Piège : `librexruntime.so` exporte aussi `__imp__<Nom>` ; le repli HLE passe par `dlsym(RTLD_NEXT)` (sinon récursion).
- Liste par défaut (mode profil XAM) : `XGetVideoMode`, `XamUserGetXUID`, `XamUserGetSigninState`, `XamUserLogon(Ex)`,
  `XamUserCheckPrivilege`, `XamUserIsOnlineEnabled`, `XamShowMessageBoxUI(Ex)`, `XamShowKeyboardUI`, `XamShowSigninUI(p)`,
  `XamShowCreateProfileUI`, `XamShowDeviceSelectorUI`, `XamShowGamerCardUIForXUID`, `XamShowGuideUI`, `XamShowAchievementsUIEx`,
  `XamShowGamerCardUI`, `XNotifyQueueUI`, les imports d'avatar
  (`kAvatarImports`) ; `DASH_XAM_REDIRECT` ajoute, `DASH_XAM_NOREDIRECT` retire. Les accroches de `dash_profile.cpp` / `dash_user.cpp`
  (`XamUserGetName`, `XamUserGetGamerTag`, `XamProfile*`, `XamReadTileEx`) passent la main à l'export de XAM (`DASH_XAM_FWD`).
  Restent au HLE : `XamUserReadProfileSettings(Ex)`/`Write` (rediriger plante : `0x7008004F` lu comme pointeur), sauf la synchro du gamertag.
- Les messages vers l'application 0xFA (XMP : `XMsgInProcessCall`, `XMsgSystemProcessCall`, `XMsgStartIORequest(Ex)`) vont à XAM ;
  les autres applications restent au HLE (`DASH_XMP_HLE=1` : tout au HLE).
- Les modules recompilés appellent `__imp__<Nom>` masqués générés (`gen_module_xam_imports.py`) qui appellent `CallModuleImport` : l'export de
  XAM de même ordinal est exécuté ; sinon repli `dlsym(RTLD_DEFAULT)` avec avertissement. Les imports `xboxkrnl` restent au noyau du SDK.
- Imports d'interface (`XamShow*UI`, `XNotifyQueueUI`, `XamResumeUpdaterUI`) : tous ont un hook ; ceux qui ne sont pas dans la liste par
  défaut restent des `REX_EXPORT_STUB` du SDK (r3 résiduel, aucune fenêtre : le SDK n'a de vraie interface HLE que pour la boîte de message,
  le clavier et le sélecteur de périphérique, déjà redirigés). Mesure (banc, appel par `DASH_TEST_UI_CALL`) : redirigés, Marketplace, Games,
  Live signup/upsell, Nui hardware/troubleshooter reviennent `0` ; amis, messages, invitations, Nui amis/jeux/guide/signin/boîte : `5` ;
  parties, Gold, code d'accès : `0x57`. Aucun n'est appelé dans le parcours normal hors ligne (Marketplace et Live demandent une connexion
  Live : la tuile ouvre déjà la boîte « Xbox LIVE » de XAM par `XamShowMessageBoxUI`) ; ils restent hors de la liste par défaut, activables
  par `DASH_XAM_REDIRECT=<noms>`.
- Signatures : `XamShowMessageBoxUI` a 9 arguments (le 9e à `r1 + 0x54`) ; `XamShowGuideUI` un seul (`user_index`).
- `XamUserGetSigninState` : `XN_SYS_SIGNINCHANGED` est rediffusé côté HLE (le Dashboard écoute les files du SDK, pas celles de XAM).
- `AvatarEditorOption2` : `XamLaunchAvatarEditor` (ordinal 2011) ouvre `XamShowAvatarMiniCreatorUI(user, 2)` (ordinal 1700) ; ouverture
  différée depuis le MiniCreator (un appel synchrone après `XamAppUnloadStack` renvoie 5).
- « Customize Avatar » de dash.xex (`sub_9231DC28`) : `XamPackageManagerGetExperienceMode` (retour `0x9231DC44`) et `XamPackageManagerGetFileSize`
  (retour `0x9231DC64`) vont à XAM recompilé pour CET appel seulement (mesuré le 2026-11-26 : mode `0x2`, `AvatarEditor.xex` 15 556 608 octets
  dans l'index) ; ailleurs le HLE (r3 = 0), car la redirection globale ouvre `AvatarEditor.xex` au démarrage. Le Dashboard appelle alors
  `XamLaunchAvatarEditor`, servi par l'option 2. Le MiniCreator n'a pas de bouton B (légende A / Y) : on ne quitte le choix qu'en choisissant.
- Après `XamAvatarSetManifest` (choix d'avatar), le manifeste donné au profil HLE est relu aux lectures suivantes de `0x63E80044` et
  `XN_SYS_PROFILESETTINGCHANGED` (0xE) est rediffusé (sinon My Xbox gardait l'ancien avatar, ou la silhouette grise pour un profil neuf).

### Rendu natif du HUD (palier N2)
- XAM embarque sa propre D3D9 2.0.2099.16 (celle du Dashboard : 2.0.20158) : groupes de registres décalés de 1088 octets, shaders à
  `+12944` (pixel) et `+12948` (vertex). Adresses invitées (`native_gpu/profiles/hud_xam13604.h`) : `BeginVertices 819C8E38`,
  `EndVertices 819C90B8`, `DrawVerticesUP 819C90D8`, `SetTexture 819C8C60`, `SetVertexShader 819C6BE8`, `SetPixelShader 819C69D0`,
  `Resolve 819CA3E0`, soumission `819CC490` (`VdSetSystemCommandBuffer`), image du HUD `sub_818A1368` (← `sub_818A1B58` ← `sub_818A2500`).
- Aucun PM4 exécuté : `src_xam_d3d/xam_d3d_hooks.cpp` (+ `xam_d3d_entries.inc`, 1347 adresses `819B0000-819F0000`) définit des `sub_<adresse>`
  forts qui appellent `hudd3d_call` ; `native_gpu/src/hud_front.cpp` est la façade (second noyau `ngpu_core`, `ngpu_core` non modifié).
  Cible effacée avant chaque image (D3D ne la vide jamais) ; soumission limitée à 60 Hz.
- Surface 852x480 relue à la soumission, puis `ngpu::HudComposite` la compose sur l'image du Dashboard en 1280x720, alpha prémultiplié.
  `ngpu::HudActive()` (16 pixels d'alpha >= 8) suspend les entrées du Dashboard jusqu'au relâchement des boutons.
- Fond atténué : le premier dessin de l'image (quad plein écran, `vs 509E…`, `ps 4D19…`, `ColorFactor` 0,22) n'est pas rendu ; son facteur
  multiplie l'image du Dashboard, dans la boîte englobante de l'overlay seulement (lecture déduite, non vérifiée sur console).
- `VdRegisterXamGraphicsNotification` / `VdCallGraphicsNotificationRoutines` (`kernel_video.cpp`) : rappel `sub_818A04F8`, messages 2
  (`X_DISPLAY_INFO`) puis 1 ; sans eux les sommets du quad étaient NaN. `VdQuerySystemCommandBuffer` rend 1 pour XAM.
- Plan overlay D1OVL (registres `0x1860-0x186B`, PM4 type 0 dans le tampon système) : notifications (« a signed out ») composées à leur
  fenêtre (244x90, bas centre) sans prendre les entrées.
- Shaders : `tools/hud_shaders.py extract|translate` : 34 conteneurs `0x102A0E0x` (29 PS, 5 VS), 33/34 traduits (échec : VS `817F9550`,
  jamais demandé). Nom = XXH3-64 du microcode, vfetch mis à zéro ; `DASH_HUD_SHADERS=<dossier>` (Dashboard : `DASH_NGPU_SHADERS`).
- Le HUD tourne en continu dès l'image 2 : sans demande, il dessine le chrome de chargement (barre + anneau) ; `DASH_HUD_CHROME=1` l'affiche toujours.

### Entrées (palier H, `common/src/kernel_xinputd.cpp`)
- XAM découvre une manette par `DrvSetUserBindingCallback(0x818AD898)` ; un fil hôte annonce la manette virtuelle `0x10000001` (rappel
  d'arrivée `0x81BE1E10 + 4 * type`), tableau des ports `0x81BFD948` (entrée `+8 + 8 * (7 * port + type)`). `XInputdGetCapabilities`
  (manette standard), `XInputdReadState` (`InputSystem::GetState(0)` du SDK, port 0 seulement), `XInputdNotify` (sans effet).
- `src_xam_patch/xam_input_patch.cpp` : `sub_818C4240` (cœur de `XamInputGetState`) lit la manette virtuelle quand XAM répond `0x48F`.
- Le Dashboard et le HUD lisent le même `InputSystem` ; l'exclusivité est côté Dashboard (voir `HudActive`). Frappes du HLE vidées tant que
  le HUD a le focus. Touches de test : `tools/xkeys.py` (`XKEYS_HOLD=0.12`).

### Stockage du profil (palier N4)
- XAM écrit un paquet CON (`Content\<XUID>\FFFE07D1\00010000\<XUID>`) et demande au pilote STFS du noyau de monter le volume. Pilote
  minimal `common/src/kernel_stfs.cpp` dont le volume est un dossier hôte : `<game_data_root>/profiles/<XUID>/` (autres paquets :
  `.stfs/`). Codes de `StfsControlDevice` : 0 début d'écriture, 1 fin, 2 vérification, 3 empreinte (36 octets, SHA-256 des fichiers), 4
  validation, 5 état. Un paquet créé ici n'est lisible que par ce noyau.
- Compte : `Account` = 24 octets (« XDASHOBF », longueur, CRC-32) + données en clair (`kernel_obfuscate.cpp`). En-tête signé par HMAC-SHA-256
  avec une clé locale tirée au hasard (`<état XAM>/local_console.key`, `kernel_localkey.cpp`). XUID aléatoire hors ligne `0xE…`
  (`sub_818AFAD0`). Aucune clé de console fabriquée, aucune protection contournée. Profil de test : supprimer `profiles/<XUID>` et `Content/<XUID>`.
- `dash_auto_profile` (défaut `false`) : sans profil, `XamUserGetName`/`GetGamerTag` rendent une chaîne vide avec succès.

### Partition système étendue (avatar)
- `signin.xex` n'appelle `XamShowAvatarMiniCreatorUI` que si `XamPackageManagerGetFileSize("Guide.AvatarMiniCreator.xex")` est non nul ; XAM
  lit son index dans `system.manifest` (signé) de `\Device\Harddisk0\SystemExtPartition\`. `MountSystemExtPartition` monte
  `<données>/SystemExtPartition` sur `\Device\HostSystemExtPartition` avec un lien (le NullDevice du SDK masquerait un montage direct) ;
  `XboxKrnlVersion` reprend celle du Dashboard.
- `tools/appimage/xenondash_import.py` (`build_system_ext`) reconstruit l'arbre depuis `$SystemUpdate` (`system.manifest` à la racine ;
  `FFFE07DF0000000{1,2,6,8}` dans `Content/0000000000000000/FFFE07DF/00008000/` ; `Xna_TitleLauncher.xex` dans `32000100/` ; le reste dans
  `20352400/`) dans `<dst>.new`, puis remplace (atomique). `AppRun --system-ext-only` répare une installation sans partition.

## 2. Paliers

| Palier | Fait | Aujourd'hui |
|---|---|---|
| A | XAM décodé (`xex_rekey.py`, `xexinfo`), codegen module, projet 13604-xam. Correctifs : flash montée avant XAM, XAM dans le processus système (`RtlEnterCriticalSection` plantait), `IoDismountVolumeByName` = succès (boucle de 8,3 M de lignes) | en place |
| B | Cause du hub noir : tables de fonctions de XAM superposées à dash.xex ; `xam_unimpl_patch.py` | corrigé par C |
| C | `function_table_base`, `PPCImageInfo::table_base`, contrôle de recouvrement en adresses physiques (`GuestRangesOverlap`), `ReserveImageAlias` | en place (tas séparés depuis N3) |
| D | Hub affiché avec XAM : `ObReferenceObjectByName` lisait mal l'`ANSI_STRING` (voir correctifs) | corrigé dans `common/` |
| E | Mécanisme de redirection des imports (`dash_xam_redirect`), `XGetVideoMode` via XAM | en place ; liste étendue en N4 |
| F | `XamShowMessageBoxUI` acceptée (`0x3E5` au lieu de `0x65B`) : 9 blocages de l'initialisation de XAM levés (voir correctifs) | en place |
| G | `XeCryptRandom` constant → boucle de la 0xFA ; `hud.xex` recompilé (module `dash_hud`) ; imports du HUD vers XAM ; `DeviceAuth` neutralisé | HUD chargé et vivant |
| H | Manette virtuelle `XInputd*` ; plantages sporadiques (en-tête d'événement périmé, `XThread` détruit trop tôt) | en place, 20 démarrages sans SIGSEGV |
| N2 | Rendu natif du HUD (boîte de message par-dessus le hub) | en place |
| N3 | `vk` et `createprofile` recompilés (huit modules) ; tas 0x8/0x9 séparés ; `regen_xam_modules.py` | en place |
| N4 | Création de profil, connexion, premier lancement sans profil, par XAM recompilé et pilote STFS local | en place |
| N5 | Boîtes du HUD appelables depuis l'hôte (`dash_xam_hud_ui.{cpp,h}`, thread invité unique, file sérialisée) ; progression de construction (`DASH_TEST_BUILDBOX`) | socle en place ; toasts, `XuiTextElementSetText` (ordinal 867) et `XamShowAchievementsUI` non faits |
| Guide (20-26 nov.) | Chaîne d'ouverture du Guide, strings localisées, boîte « Unrecognized Disc » retirée | le Guide s'ouvre (section 5) |

## 3. Correctifs et causes

### Noyau et runtime (`common/`, `dashboards/2.0.13604.0-xam/`)
| Correctif | Cause |
|---|---|
| `ObReferenceObjectByName` (`kernel_stubs.cpp`) | le SDK lit l'`ANSI_STRING` comme une chaîne C et n'écrit pas `*objet` en échec : `sub_81883CB8` déréférence `0xBEBEBEBE` ou un objet ancien, ce qui libère le handle du thread du Dashboard (`~XThread` dans `ExCreateThread_entry`, `pthread_cond_destroy` bloqué : hub noir). Échec : `*objet = 0`, `STATUS_OBJECT_NAME_NOT_FOUND` |
| `UsbdBootEnumerationDoneEvent` type 0 (`FixKernelVariables`) | créé en type 1 (auto-réarmement) : le premier fil (`sub_81872180`) consomme le signal ; `sub_81879B58` attendait `0x30000254` |
| `DrvGetContentStorageNotification` = `0xC0000001` | stub à r3 inchangé : `sub_8185C5E0` bouclait à 100 % CPU |
| minuteries invitées (`kernel_timers.cpp`) | `KeSetTimerEx`/`KeCancelTimer` stubs, objet `0x81BFB6F0` (type 9) refusé par `GetNativeObject` : `sub_81890DE8` recevait `0xC000000D` en boucle ; `KeSetTimerEx` transmet le DPC |
| garde audio (`kernel_audio_guard.cpp`) | `AudioSystem::SubmitFrame` avec client nul pendant une désinscription ; `XAudioGetDucker*` stubs (f1 = 1.0, SIGFPE `sub_81BAB930`) |
| `ObCreateObject`/`ObInsertObject`/`ObReference(Handle)` (`kernel_ob.cpp`) | stubs : `sub_8187D9A8` (type `'Nfto'`) recevait un objet nul, l'application 0xFD échouait. Corps jamais libéré |
| `XboxHardwareInfo.Flags |= 0x200` | `sub_818A2F90` (HUD) n'est planifiée que si le bit est posé (valeur `0x220`) |
| `KeGetCurrentProcessType` = 2 pour XAM | XAM existe en une instance, système ; l'état « titre » (id 238, tas de classe 0) n'existe pas |
| `XeCryptRandom` (`kernel_random.cpp`) | le SDK fait `memset(0xFD)` : le tirage par rejet de `sub_81A71EE8(0, 0x89ABCDEF)` bouclait (156 024 appels en 30 s) ; ce n'étaient pas les clés |
| `HalGetPowerUpCause` octet 4 bit 0 = 0 ; `XexLoadExecutable` = 0 | `sub_81892A60` lit `0x81BFB854+4` : bit à 1 = `VdDisplayFatalError` `0x12348` (lr `0x81872278`) ; `STATUS_DLL_NOT_FOUND` `0xC0000135` fatal en `sub_8187A038` (`0x12247`, lr `0x8187A2F8`) |
| `NicGetLinkState`, `MtpdGetCurrentDevices` | stubs à r3 aléatoire : boucle réseau du HUD. `NicGetLinkState` : ne JAMAIS écrire par r4 (r4 hérité), r3 = 0 seulement |
| `XeKeysGetStatus` | sortie `*r3 = 0x4` (`DASH_XEKEYS_STATUS`) : à 0, XAM ouvre la boîte « Game Error » ; à `0xC`, le fil du HUD ne démarre pas |
| `kernel_event_guard.cpp` | XAM réutilise la mémoire d'objets sans `KeInitializeEvent` : signature `REX\0` périmée, `KeResetEvent` plante (`sub_81890650`) |
| `ObReferenceObject`/`ExCreateThread`/auto-déréférencement (`kernel_ob.cpp`) | `ObReferenceObject` stub, drapeau `0x80` sans référence, `XThread::Exit` libère avant `Thread::Exit` : `XThread` détruit trop tôt. Fuite de quelques centaines d'octets par fil |
| `VdSetSystemCommandBuffer`, `VdQuerySystemCommandBuffer` (`kernel_video.cpp`) | stub journalisé 371 443 fois ; l'interrogation sans pause tenait un fil à 100 % |
| `game:` / `\Device\CdRom0` | `GAME:` → `\Device\CdRom0` (posé par XAM) pendait : `game:\flash\<nom>` échouait une fois sur deux ; CdRom0 monté sur un dossier VIDE ; SDK `766678d`, `0bc075f` : liens sensibles à la casse |
| `XamPackageManagerGetExperienceMode` = 0 | stub à r3 résiduel ; vers XAM pour « Customize Avatar » seulement (`DASH_XAM_EXPERIENCE_REDIRECT=1` : partout, ouvre `AvatarEditor.xex` au démarrage) |
| `XamIsIptvEnabled` (export 1301, `RETAIL_FLAGS & 0x1000`) | le stub laissait r3 ; `EcoMediaroomEnabled` ajoutait un emplacement Mediaroom vide dans My Xbox |
| `XamLoaderGetDvdTrayState` sans argument (`sub_81884580`) | l'ancien prototype écrivait 2 par r3 dans la vtable de l'Async Loader : appel nul dans `sub_922AA018` (lr `0x922AA0D8`) ; trouvé par point d'arrêt matériel |
| `MmDoubleMapMemory`/`MmUnmapMemory` | vrai double mappage (`Memory::MapAlias`/`UnmapAlias`) |
| `NetDll_WSAStartupEx` = `NetDll_WSAStartup` | `signin.xex` l'appelle par `XexGetProcedureAddress(xam, 36)` ; le stub rendait 2 : « temporarily unavailable, 80004005 » |
| `Rtl*String`, `ObTranslateSymbolicLink`, `NtOpenSymbolicLinkObject`, `XeKeysObfuscate`, `StfsCreateDevice`, `XeCryptMd5`, `Ex*XConfigSetting` | création de profil (voir ci-dessous) |
| RMW XConfig (`kernel_xconfig.cpp`) | `ExReadModifyWriteXConfigSettingUlong` part de la valeur du SDK : Getting Started effaçait le bit `0x40` « console configurée » et déconnectait les profils |
| Fuseau horaire (`kernel_xconfig.cpp`, `host_timezone.h`, `timezone_table.h`) | XConfig 3/1..7 depuis l'hôte, table cherchée dans l'image de tout Dashboard ; noms vides en 17559 (ils font planter Clock) |
| Trap non fatal (SDK `function_dispatcher.cpp`, `f2c7948`) | appel indirect invité vers 0 avortait le processus (sortie de Game Library) ; journal borné, `r3 = 0` |
| Journal sans `[error]` (SDK `dec33681`) | périphériques/modules absents, XLIVEBASE hors ligne : un avertissement par cas ; `tools/banc_suite.py` échoue sur toute ligne `[error]` |
| Fibres de XAM (`src_xam_patch/xam_fiber_switch.cpp`) | `sub_819BF2E0` recompilé revenait sur la pile hôte de l'appelant (fil à 99 % CPU) ; vraie bascule ucontext, pile mmap gardée ; `DASH_XAM_FIBER_FIX=0` désactive |
| `DASH_HUD_NOTIFY` en opt-in | la diffusion `XN_SYS_UI` sur les pixels du chrome re-posait le bit « interface active » : Guide bloqué sur son panneau de chargement |
| Frappes pendant le Guide | le A qui connecte un profil ouvrait ensuite la tuile du Dashboard : file du HLE vidée tant que le HUD a le focus |

Création de profil (N4), causes successives : `0x8000FFFF` (`RtlAppendStringToString` stub, `sub_8185CD38`) ; `0x80070002` (liens `\System??\__tmpN:` avec
préfixe) ; `0x80070005` (`Partition1` en lecture seule : `allow_game_relative_writes=true` en mode XAM) ; `0xC0000022` (`CreatePath` du SDK) ; nom de
paquet vide (`%I64X`, SDK `format.h`) ; `Account` = octets de pile (`XeKeysObfuscate`) ; `0x80070570` (en-tête jamais signé, descripteur STFS sans `0x24`).

Interface Xbox seule (To-Do 1 et 4) :
- `XNotifyQueueUI` par XAM échouait en `0x8007000E` : l'export crée la notification (`sub_8187F6D0`) et exige une tâche XAM sur le fil appelant
  (`XamTaskGetCurrentTask`), or `KeGetCurrentProcessType` (crochet de `dash_xam_runtime.cpp`) renvoie 2 pour tout appelant dans l'image de XAM.
  Le dispatcher passe l'appel à `SetTitleCall(true)` : le crochet répond alors 1 pour le seul `lr 0x8187BAC4`, XAM programme la création par
  `XamTaskSchedule`. Toast affiché (plan D1OVL du compositeur), y compris sans profil connecté.
- La boîte d'erreur de démarrage (« Unrecognized Disc / This disc is unreadable » ou « Game Error / The game couldn't start » selon les
  démarrages) restait cachée sous la scène de repos du HUD ; le premier toast (scène empilée) la révélait. Cause : le chargeur de titres de XAM
  (`sub_818767A8` -> `sub_818765F8`) signale un échec de chargement (statut `0xC000000F`, lecteur vide) et poste le message `0x2100D`
  (`sub_818B5A28` -> `XamShowMessageBoxUI`). Mesures : supprimer le message à la source (patch de `sub_818765F8`) empêche le second chargement
  de `hud.xex` au démarrage, le Guide s'ouvre trop tard (suite KO : guide, signin, signed_in) ; refuser l'affichage (`XamShowMessageBox` du HUD,
  r3 = 0) laisse une scène vide que le toast montre (cadre et fond atténué). Retenu : laisser la boîte s'ouvrir et la fermer comme l'utilisateur,
  par un A donné au HUD 1,5 s après son ouverture (`XamInputGetKeystrokeHud`, `g_boot_box_ms` de `dash_xam_redirect.cpp`) ; seules les boîtes de
  titres et textes connus (`IsBootErrorBox`) demandées par `hud.xex` ; `DASH_HUD_MSGBOX_KEEP=1` la laisse ouverte. Remplace le refus par `lr`
  `0x913EC15C` (plus jamais atteint), retiré. Hypothèses mesurées et écartées : contenu ouvert par `sub_81B17C98` (message `0x20002`), message
  de démarrage `0x20023` (`DASH_XAM_NO_BOOTBOX=1` ne retire pas la boîte).
- `XamShowAchievementsUIEx` et `XamShowGamerCardUI` par XAM : envoient `0x21008` à l'application 254 (gamerprofile.xex) ; panneau « Achievements »
  et fiche « Profile » affichés (profil connecté, 0 succès). Redirigés par défaut.
- Progression de construction (`dash_xam_hud_ui.cpp`) : le texte d'une boîte ouverte de XAM est figé à sa création ; l'avancement passe donc par
  une notification (`ShowXamToast`, second fil invité car le premier est bloqué par la boîte) à chaque étape ou tranche de 10 %. `DASH_TEST_BUILDBOX=progress`
  simule une construction de 6 étapes (état fictif lu par le même surveillant que la vraie, jamais de simulation de disque).

### SDK (`third_party/xenondash-sdk`)
- `7348c20` : `AccessViolationCallback` refusait les tas `kGuestVirtual`. `core/memory_posix.cpp` : `memfd_create` d'abord (voir pièges).
- `InputSystem::devices_mutex_` : `RefreshDevices()` sans verrou, appelé par `GuideFrame` et les fils invités (plantage `std::stable_sort`).
- `XHostThread::Execute` appelle `fpscr.InitHost()` (sinon `MXCSR = 0`, exceptions flottantes démasquées : SIGFPE dans `sub_81A06F88`).
- `xex_module.cpp` : modules de ressources sans PE. `xmemory.cpp` : tas 0x8/0x9 séparés. `virtual_file_system.cpp` : `OpenFile` sous le parent résolu.
- `keybind_guide = "G"` par défaut. `7151265f` : `RtlCompareMemoryUlong` compte par octets (sans lui, « Enter Pass Code »). `69764e99` : `vavguw`,
  `crmove`, `crnot`. À corriger dans le SDK puis retirer les hooks de `common/` : `ObReferenceObjectByName`, `XeCryptRandom`, minuteries,
  `ObCreateObject`, `XThread::Exit`.

## 4. Variables d'environnement

| Variable | Effet |
|---|---|
| `DASH_XAM_MODULE` | `2` (défaut de `main.cpp`) module + point d'entrée ; `1` module seul ; une valeur déjà posée l'emporte |
| `DASH_PROFILE_MODE` | `xam` (défaut) : profils et redirections de session par XAM |
| `DASH_XAM_STATE` | dossier d'état des périphériques factices |
| `DASH_XAM_REDIRECT` / `DASH_XAM_NOREDIRECT` | ajoute / retire des imports redirigés (liste de noms) |
| `DASH_XAM_SYNC`, `DASH_XAM_FIBER_FIX=0`, `DASH_XAM_INPUT_PATCH=0`, `DASH_XAM_GFXNOTIFY=0` | retour au comportement antérieur (diagnostic) |
| `DASH_XAM_EXPERIENCE_REDIRECT=1`, `DASH_XMP_HLE=1`, `DASH_VIDEO_DISCOVERY=0` | `GetExperienceMode` vers XAM ; messages XMP au HLE ; stubs vidéo du SDK |
| `DASH_XEKEYS_STATUS=<hex>` | valeur de `XeKeysGetStatus` (défaut `0x4`) |
| `DASH_XAM_TRACE_MODULE=<n>`, `DASH_XAM_EXPORTS=<ordinaux>`, `DASH_XAM_ALT=<nom>` | trace `[xamtrace]` ; adresse invitée des exports ; charge un autre module à la place de XAM |
| `DASH_HUD_SHADERS`, `DASH_NGPU_SHADERS` | dossiers de shaders traduits du HUD / du Dashboard |
| `DASH_HUD_NOTIFY=1` | diffuse `XN_SYS_UI` aux titres (off : bloque le système UI) |
| `DASH_HUD_DISABLE`, `DASH_HUD_NOIDLE`, `DASH_HUD_NODIM_SKIP`, `DASH_HUD_NODRAW`, `DASH_HUD_CHROME`, `DASH_HUD_MSGBOX_KEEP`, `DASH_XAM_NO_BOOTBOX` | diagnostics du rendu et des boîtes du HUD |
| `DASH_HUD_RESOLVES`, `DASH_HUD_EVLOG`, `DASH_HUD_DUMP`, `DASH_HUD_STATS`, `DASH_HUD_DRAWRANGE`, `DASH_HUD_TEXDUMP`, `DASH_HUD_CALLERS`, `DASH_HUD_D3D_TRACE|SEQ|SEQT|DEVDUMP|PROBE` | traces et dumps du HUD (`hud_front.cpp`, `hud_d3d_trace.cpp`) |
| `DASH_TRACE_VD`, `DASH_TRACE_VD_SCAN`, `DASH_TRACE_VD_DUMP` | descripteur et contenu PM4 du tampon système |
| `DASH_GUIDE_TRACE=1`, `DASH_LOGON_TRACE=1`, `DASH_WATCH_APPS=1` | traces de l'ouverture du Guide, de la tâche de connexion (`sub_818B2BF8`), de l'application active `0x817E4B64` |
| `DASH_TEST_MSGBOX`, `DASH_TEST_KEYBOARD`, `DASH_TEST_SIGNIN`, `DASH_TEST_CREATEPROFILE`, `DASH_TEST_GUIDE`, `DASH_TEST_BUILDBOX=start|fail|progress` (secondes ; `_WAIT` borne l'attente), `DASH_TEST_UI=<s>` + `DASH_TEST_UI_CALL="Import:arg,arg,w:texte;..."`, `DASH_TEST_PEEK=<adresse>` | déclenchent une fenêtre du HUD sans manette (`DASH_TEST_UI_CALL` appelle n'importe quel import hooké) ; mémoire invitée |
| `DASH_AUDIO=nop`, `DASH_RETURNED_FROM`, `DASH_XENIA` | audio muet ; titre dont on revient ; binaire Xenia |
| `REX_LOG_THREADS`, `REX_LOG_DLLMAIN` | instrumentation du SDK (création de fils, `DllMain`) |

Reproduire un essai : `cd dashboards/2.0.13604.0-xam && cmake --preset linux-amd64-relwithdebinfo && cmake --build out/build/linux-amd64-relwithdebinfo -j 6`
puis `DASH_DISPLAY=92 DASH_RUNS=<dossier> DASH_ENV="…" ./run_headless.sh 40 essai` (`input_test.sh` pour les touches ; `DASH_ROOT` remplace
`--game_data_root=assets`). Le plugin GPU `xenos` n'est pas utilisé (`DASH_GPU_ARG=` vide). gdb : `-batch`, `handle SIGSEGV nostop noprint pass`,
SIGINT envoyé à gdb (ptrace interdit). Journal complet : `DASH_ARGS=--log_flush_interval=1`.

## 5. État actuel

### Profil (N4)
Au banc : sans profil, tuile « Sign In / No Profiles Found » ; A ouvre « Create Profile », `createprofile.xex` + clavier `vk.xex`, création
effective (`XamProfileCreate` puis `XamProfileGetCreationStatus` = 0), `XamUserLogon`, profil visible dans My Xbox. Redémarrage : « 1 Profile
Found », liste « Player1 / HDD ». Connexion par Sign In sans code d'accès (« Enter Pass Code » vient d'un SDK sans `7151265f`) ; gamertag
sur le XMB (`XamUserReadProfileSettings` synchronisé avec le profil HLE, `cd7f3b0`). `DASH_TEST_SIGNIN` ouvre la vraie fenêtre de `signin.xex`.

### Guide
Chaîne : G ou bouton Guide → `ShowXamGuide` (`dash_xam_redirect.cpp`) → `XamShowGuideUI` (ordinal 772, `0x81898630`) → portier `sub_8189F0D0`
→ message `0x20004117` à l'application 254 (`sub_8187B960`) → répartiteur `0x8000000B`/`0x80000007` (requête `0x402E9B80`, scène en +4) →
chargeur `sub_8189BE20` (scène `0x21015`) → ouvreur `sub_818A21B0("hud.xex")`. Le portier refuse (r3 = 5) tant que le mot `0x81BFCAE8` (masque de
bits : 0 → 4 au boot → 0) est non nul ; `ShowXamGuide` le remet à 0 le temps de l'appel (toujours nécessaire). L'ouvreur exige
`UIstate(0x81BFCAE8)+172 != 0`, `+176 != 0`, `+216 == 0` ; avec `XeKeysGetStatus = 0x4` le fil du HUD (`sub_818A2F90` → `sub_818A2C98`) les pose
lui-même (plus de `0x80000012` à la main). Le Guide s'affiche (lames Marketplace/Games/Home/Media/Settings, Sign In, horloge locale) et les boîtes
système se ferment au bouton B. Le Guide et le HUD sont inséparables (`DASH_HUD_DISABLE=1` : accepté mais invisible).

### Avatar (banc, 2026-11-24)
- L'avatar choisi s'affiche en couleur sur le XMB. Causes de la silhouette grise, toutes corrigées : `LDI*` (LZX) stubs (`xam_kernel_stubs.cpp`,
  `243b35e`) ; `vavguw`/`crmove`/`crnot` absents du recompilateur (`69764e99`) ; `vfetch` indexé des os ignoré par XenosRecomp (patch
  `0009-dash13604-indexed-vfetch.patch`, `105b468`) ; hauteur de texture résolue `y + destHeight` sous tuilage prédit (`ngpu_core/src/renderer.cpp`, `687ecab`).
- Option 2 (retenue) : choix d'avatar à la place de l'éditeur ; réglage `0x63E80044` (1000 octets) écrit dans `profiles/<XUID>/FFFE07D1.gpd`.
  Option 1 (éditeur complet recompilé) : retirer l'appel à `AvatarEditorOption2`.
- Carte d'ombre noire (passes de profondeur seule ignorées) : sans effet visible, pas de correctif. `XexLoadImage` refuse les XEX d'avatar non recompilés
  (sinon le Guide affichait un panneau vide).

### Médias et Video Library
Les messages XMP vont à XAM : la scène `MediaSourceSelection.xur` (`dashcommon.xzp`) envoie `0x0007002B`, inconnu du HLE (X_E_FAIL, `0x8007065B`, retour au canal
Welcome). Application XMP : table de `sub_81893EE0` (`0x81C06F78 - 0xB8 * id`), objet `0x81C07688`.

## 6. Pièges
- `/dev/shm` de 64 Mo (conteneur) : l'arène invitée (4,5 Go, `MAP_SHARED`) donne un SIGBUS par écriture au-delà (1,6 M de fautes, écran noir) ; SDK : `memfd_create`.
- Instrumentation : lire `ctx.lr`/`r3` à l'entrée de la fonction, pas après un `bl` ; adresses PowerPC par `lis`/`addi` (pas de `grep` du littéral) ;
  mesurer à l'exécution plutôt que recalculer. Les objets en `0x40…` sont en vue physique (`TranslatePhysical`) : le descripteur de `VdSetSystemCommandBuffer` donne une adresse physique.
- gdb : `handle SIGSEGV nostop noprint pass` (suivi de pages du SDK) ; watchpoint sur l'alias hôte (`0x182000D64`), pas `0x192000D64`.
- Bibliothèque SDK : `RUNPATH=$ORIGIN` sans copie à côté : prendre `sdk/lib/` ; une copie périmée régresse (« Enter Pass Code »).
- `DASH_PROFILE_MODE=xam` seul ne suffit pas : il faut aussi `DASH_XAM_MODULE` ; sans module, le Dashboard répond par le HLE (une capture « qui marche » peut être du HLE).
- `Partition1` et `game:` masquent la flash (même point de montage) ; `GAME:` (lecteur) et `game:` (titre) sont deux volumes distincts.
- Déchiffrement `.xex` : fichiers d'autres versions inutilisables (un `.xexp` est un delta). Hooks de `common/` liés à tous les Dashboards ; ceux qui dépendent de XAM
  ne s'activent que pour un appelant dans l'image de XAM.
- `XeKeysVerifyRSASignature` rend TRUE en dur (compromis accepté ; le SHA-1 du contenu reste vérifié) : 5 appels au démarrage (appelants `0x8185A4C4`, `0x818BF0C0`).
  `XexLoadExecutable` (lr `0x818783AC`) lit un nom vide : ne pas le faire échouer avant de corriger la lecture. `XexStartExecutable` (lr `0x8187A028`) exécute la fonction sur un fil système.
- Instance unique de XAM : tout ce qui dépend de l'état « titre » (nom du titre en cours, messages par processus) ne peut pas fonctionner.
- Les lignes `[ngpu]` du rendu natif sont sur stderr, jamais dans `dash.log` ; `dash.log` est tronqué sans `--log_flush_interval=1`.

## 7. Problèmes ouverts
- Mesuré le 2026-11-26 (banc) : connexion d'un profil sans avatar (Guide, MiniCreator, choix, Guide de retour), sous-menus de Customize Profile
  (image, nom, avatar), création de profil jusqu'à « Save Profile » (scène Congratulations de `createprofile.xex`, après le choix d'avatar),
  panneau du Guide après B : tout fonctionne ; la fermeture du Guide et le panneau vide venaient du MiniCreator non recompilé (voir plus haut).
- MiniCreator d'option 2 sans bouton B ; avatar complet (`AvatarEditor.xex`) à recompiler ; créer un profil nommé `a` (déjà existant) affiche « must begin with a letter » (hypothèse : code d'erreur « nom pris » mal traduit ; non mesuré).
- AppImage avec ces correctifs non vérifiée sur le Deck ; entrées manette inégales (SDL voit les manettes, boutons parfois absents du hook `XInputdReadState`).
- `XamUserReadProfileSettings(Ex)`/`Write` toujours au HLE ; contenu lié au profil HLE. Mesure : `DASH_XAM_REDIRECT=XamUserReadProfileSettings,XamUserReadProfileSettingsEx`
  (lecture par XAM) donne gamertag et avatar corrects, mais l'image du joueur (haut à droite) disparaît (hypothèse : un réglage lu par le hub, ex. `0x402C0011`, absent du GPD) :
  désactivé par défaut tant que ce réglage n'est pas rempli.
- Rendu du HUD : constantes booléennes de pixel shader (`+0x2780`/`+0x2790` supposés, `CopyAlpha` `817E59F0`/`817E5B70` non vérifiés), gamma, MSAA,
  plans de coupe ; cadence ≈ 34 images/s sous lavapipe (`ReadResolved` + `WaitIdle`) : le HUD devrait attendre un vsync.
- Le texte d'une boîte de message ouverte ne se met pas à jour (`XuiTextElementSetText` non essayé : l'avancement passe par des notifications) ; le toast recouvre l'étiquette du bouton « Masquer » pendant son affichage.
- Achievements : le Guide et la fiche profil s'ouvrent, mais le bouton « Achievements » du hub n'a pas été relié (`kActionAchievements` de `dash_guide_actions.cpp` journalise seulement) ; succès débloqués par un titre jamais testés (aucun profil avec succès au banc).
- Lecteur optique réel exposé en `\Device\CdRom0` ; lecture d'un média ; sources Computer, Portable Device, Current Disc.
- Défauts du SDK contournés dans `common/` (liste en section 3) ; réservations d'alias non sauvegardées ni libérées au déchargement ; `BaseHeap::Reset` efface la table de pages à chaque XEX chargé.
- `signin`/`createprofile`/`gamerprofile` en même temps : impossible (bases identiques).

Historique détaillé avant condensation : `git show d96c252:docs/xam_recompile.md`
Historique détaillé avant condensation : `git show d96c252:docs/xam_13604_compilation.md`
Historique détaillé avant condensation : `git show d96c252:docs/xam_etat.md`
Historique détaillé avant condensation : `git show d96c252:docs/avatar_hub.md`
