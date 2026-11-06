# Rendu natif au niveau D3D (zéro émulation du GPU) : étude de faisabilité

Date : 2026-11-06. Périmètre : dash.xex **2.0.6770.0** (Blades) analysé en détail ; 13604 (NXE) et 17559 (Metro)
extrapolés à partir de la 17559 déjà portée (partiellement) et des images PE présentes dans `work/port/`.
Méthode : analyse statique de `work/dash.pe` (image décompressée), du code généré, des imports du noyau et des
sources publiques lues en ligne (aucun dépôt cloné, aucun code tiers exécuté). Outil : `tools/analyse_d3d.py`
(Python pur, lecture seule ; sous-commandes `shaders`, `shadernames`, `familles`, `cf`, `funcs`, `who`, `api`, `pm4`,
`strings`).

Les noms de fonctions D3D ci-dessous sont des **identifications probables** (motifs d'instructions, décalages
dans la structure du périphérique, tampons PM4 émis, importations `Vd*`, chaînes de diagnostic). Le niveau de
confiance est indiqué (H = forte, M = moyenne, F = faible). Rien n'a été vérifié à l'exécution.

## 1. Conclusion

**Faisable, avec un périmètre bien plus petit qu'il n'y paraît**, mais pas « gratuit » :

1. Le Dashboard 6770 embarque une **bibliothèque Direct3D 9 de la XDK liée statiquement** (version 2.0.5526 du
   compilateur, chaînes `ERR[D3D]: ... GPU is hung`, `D3D: GPU initialization (HSIO training)`). Elle occupe
   `0x92120000-0x92140000` environ : **350 fonctions, 109 572 octets**, dont **110 points d'entrée** appelés de
   l'extérieur et **93 utilisés par le code du Dashboard hors décodeur vidéo**. C'est la surface à remplacer
   (REX_HOOK) ; le reste (bas niveau : anneau de commandes, PM4, `Vd*`) disparaît.
2. Les **shaders sont des conteneurs D3D (Xenos microcode) en clair dans `.rdata`** : 221 conteneurs, repérables
   par le fanion `0x102A1100/0x102A1101`. Ce format est exactement celui que XenosRecomp lit. Il n'y a **aucun
   shader dans `shrdres.xzp`** ni dans les autres ressources contrôlées (voir §4).
3. Le gros du volume de shaders (**151 sur 221**) appartient au **décodeur WMV (dxva_pk)** qui utilise de
   l'**export mémoire (memexport)** dans des vertex shaders : **c'est le vrai point dur** (XenosRecomp ne gère pas le
   memexport). Les shaders propres à l'interface (XUI + effets) ne sont que **70** (18 VS, 52 PS), ce qui est
   très modeste. Recommandation : remplacer le décodeur WMV matériel par un décodeur logiciel natif (ffmpeg) plutôt
   que de traduire ses shaders.
4. Aucune des deux briques publiques n'est réutilisable telle quelle : XenosRecomp est sous **licence MIT** mais
   « ne fonctionne pas out of the box » (de son propre aveu), et le code de rendu d'UnleashedRecomp est sous
   **GPL-3.0** (incompatible avec une reprise de code si notre projet n'est pas GPL-3 lui-même). plume (RHI) est MIT.

Estimation : **~55 jours-personne pour la 6770**, **+15 j pour la 13604**, **+18 j pour la 17559**, soit
**≈ 86 jours** au total (détail au §7). Ordre de grandeur, avec incertitude de ±30 % tant que le Resolve/EDRAM
et le memexport ne sont pas tranchés.

## 2. Inventaire D3D dans dash.xex 6770

### 2.1 Imports noyau `Vd*` / `Mm*`

38 imports `Vd*` sont déclarés ; **aucun n'est appelé directement par le code de rendu** : tous sont concentrés
dans la couche d'initialisation/affichage de D3D et dans le code de gestion du mode vidéo. C'est précieux :
la « frontière GPU » est étroite.

| Import | Appelants (fonctions) | Rôle |
|---|---|---|
| `VdSwap` | 1 (`0x92121950`) | présentation d'image (commande de swap vers le scaler) |
| `VdInitializeRingBuffer` | 1 (`0x9212ADE0`) | création de l'anneau de commandes PM4 |
| `VdEnableRingBufferRPtrWriteBack` | 1 (`0x9212ADE0`) | |
| `VdSetSystemCommandBufferGpuIdentifierAddress` | 2 (`0x9212ADE0`, `0x9212D6F0`) | adresse du « GPU identifier » (fences) |
| `VdInitializeEngines` / `VdShutdownEngines` | 1 / 2 (`0x9212D3A0`, `0x9212D6F0`, `0x9212CD30`) | démarrage/arrêt du GPU |
| `VdSetGraphicsInterruptCallback` | 2 (`0x9212D3A0`, `0x9212D6F0`) | interruptions GPU |
| `VdInitializeScalerCommandBuffer` | 1 (`0x92132140`) | tampon de commandes du scaler (sortie vidéo) |
| `VdGetSystemCommandBuffer`, `VdPersistDisplay` | 1 / 2 | tampon système, affichage persistant |
| `VdRetrainEDRAM`, `VdRetrainEDRAMWorker` | 1 (`0x921323C0`) | entraînement matériel de l'EDRAM |
| `VdIsHSIOTrainingSucceeded` | 1 (`0x9212D490`) | entraînement HSIO |
| `VdQueryVideoMode`, `VdQueryVideoFlags`, `VdEnumerateVideoModes`, `VdGetCurrentDisplayInformation`, `VdGetCurrentDisplayGamma`, `VdSetDisplayMode`, `VdSetDisplayModeOverride` | 2 / 1 / 2 / 3 / 1 / 1 / 8 | modes vidéo (XGetVideoMode, résolution) |
| `VdEnableHDCP`, `VdRegisterHDCPNotification`, `VdSetCGMSOption`, `VdSetMacrovisionOption`, `VdSetWSSOption`, `Vd*ClosedCaption*` | 6 / 2 / 2 / 3 / 3 / 1-2 | protection/ordre vidéo (à neutraliser) |
| `VdEnableDisableClockGating`, `VdCallGraphicsNotificationRoutines` | 1 / 1 | divers |
| `MmAllocatePhysicalMemory[Ex]`, `MmFreePhysicalMemory`, `MmGetPhysicalAddress` | 1+4 / 4 / 4 | mémoire physique write-combined des tampons GPU |

### 2.2 Bibliothèque D3D liée statiquement : table adresse -> fonction

« Appelants » = nombre de **sites d'appel (fonctions distinctes)** hors bibliothèque D3D. « dont WMV » = part due au
décodeur vidéo (`0x92540000-0x92600000`), à ne pas compter si on remplace le décodeur WMV.

| Adresse | Fonction D3D probable | Rôle / indice d'identification | Appelants (sites / fonctions) | dont WMV | Conf. |
|---|---|---|---|---|---|
| `0x9212B950` | `D3DResource::Release` | décrémente le compteur à `+4` (lwarx), appelle le destructeur `0x9212B290` à 0 | 407 / 138 | 79 | H |
| `0x9212B8D8` | `D3DResource::AddRef` | incrément atomique de `+4` | 42 / 28 | 10 | H |
| `0x92123F20` | `D3DDevice_SetTexture` | tableau `Textures[]` à l'index 3134, mise à jour des 6 mots de la *fetch constant* (stride 24) | 127 / 52 | 25 | H |
| `0x921240A0` | `D3DDevice_GetTexture` | AddRef + retour de `Textures[stage]` | 10 / 9 | 0 | H |
| `0x92126A90` | `D3DDevice_Get*` (RenderTarget/DepthStencil/StreamSource) | AddRef + retour d'un pointeur d'état | 34 / 33 | 1 | M |
| `0x92125D18`, `0x92125EA8` | `D3DDevice_SetSamplerState_*` (paire : filtres/adressage) | modification des mots de *fetch constant* sans toucher à la ressource | 46 / 29 chacune | 14 | M |
| `0x9212E4E8` | `D3DDevice_SetVertexShader` | pointeur à `+12684`, bit « dirty » 0x10 | 95 / 50 | 23 | H |
| `0x9212E7E8` | `D3DDevice_SetPixelShader` | pointeur à `+12688`, bit « dirty » 0x08 | 88 / 52 | 34 | H |
| `0x9212E2D8`, `0x9212E3C0` | `D3DDevice_SetVertexShaderConstantF` / `SetPixelShaderConstantF` | copie de registres 16 octets, `dcbt`, base `+120`/`+376` | 16 / 13 ; 37 / 16 | 5 ; 2 | M |
| `0x92126858` | `D3DDevice_SetStreamSource` | écrit les *vertex fetch constants* (offset, taille, stride) | 36 / 18 | 14 | M |
| `0x92126A00` | `D3DDevice_SetIndices` / `SetVertexDeclaration` (à distinguer) | | 8 / 7 | 0 | F |
| `0x9212EBD0` | `D3DDevice_SetRenderState_*` (variante couleur/écriture) | champ packé à `+10920` (rlwimi) | 26 / 13 | 18 | M |
| `0x9212EE10`, `0x9212ED00` | verrous de ressources ou `Begin/End` de tampons (appel indirect par pointeur global à `+17956`/`+17960` d'une table) | | 83 / 26 ; 42 / 20 | 68 ; 5 | F |
| `0x92123BD8` | `D3DDevice_CreateTexture` | alloue 52 octets (taille de `D3DTexture`) via l'allocateur étiqueté `0x9258F7D0`, 8 paramètres | 42 / 26 | 3 | M |
| `0x92123CF8` | `D3DDevice_CreateVertexBuffer` / `CreateIndexBuffer` | alloue 48 octets, 4 paramètres | 28 / 26 | 5 | M |
| `0x92123E20` | `D3DTexture_*` (`GetLevelDesc`/`LockRect`) | | 21 / 20 | 2 | F |
| `0x921350A0` | **dessin** (`DrawVertices` / `DrawIndexedVertices`, fonction commune de validation + émission `DRAW_INDX_2`) | 3652 octets ; émet `INVALIDATE_STATE`, `IM_LOAD_IMMEDIATE`, `DRAW_INDX_2`, `EVENT_WRITE` | 31 / 27 | 5 | M |
| `0x92128D20` | `D3DDevice_Resolve` | 10 paramètres dont un flottant (ClearZ), `EVENT_WRITE` ; **seulement 2 appelants** | 2 / 2 | 0 | H |
| `0x92121950` | `Swap` interne -> `VdSwap` | 1428 octets, seul appelant de `VdSwap` | 4 / 4 | 2 | H |
| `0x92121EE8` | `D3DDevice_Swap` / `Present` (puis attente de fence) | copie les paramètres de présentation, appelle `0x92121950` et `0x921350A0` | 15 / 11 | 0 | M |
| `0x9210E2D8`, `0x9210E9A0` | assistants du Dashboard : `Resolve` + `Swap` (changement de mode, écran intermédiaire) | seuls appelants de `Resolve` | 4 ; 10 | 0 | M |
| `0x9212ADE0` | initialisation de l'anneau de commandes (`ME_INIT`, `VdInitializeRingBuffer`) | | 3 / 3 | 0 | H |
| `0x9212D3A0`, `0x9212D6F0` | démarrage / arrêt des moteurs graphiques (`Vd...Engines`) | | 1 ; 1 | 0 | H |
| `0x9212D490` | `Direct3D_CreateDevice` (entraînement HSIO, XBMovie) | chaînes `GPU initialization (HSIO training)` | 1 | 0 | H |
| `0x92132A58` | allocations d'initialisation du périphérique (3300 octets, `MmAllocatePhysicalMemoryEx`) | | 1 | 0 | M |
| `0x92133C18` | amorçage du périphérique (premier dessin + `EVENT_WRITE`) | | 1 | 0 | F |
| `0x92129EB0` | `D3DDevice_RunCommandBuffer` (`INDIRECT_BUFFER`) | | 4 / 3 | 0 | M |
| `0x9212F3A8`, `0x9212F488` | `D3DDevice_InsertFence` / `SetFence` (`EVENT_WRITE_SHD`) | | 1 ; 1 | 0 | M |
| `0x9212A4E0`, `0x9212B478`, `0x9212B818`, `0x9212A098` | attente de fence / `BlockUntilIdle` (`WAIT_REG_MEM`) | | 3-4 chacune | 0 | M |
| `0x9212C108`, `0x9212C210`, `0x9212C478` | passes GPU « bas niveau » du décodeur vidéo (`EVENT_WRITE`, `DRAW_INDX_2`, `WAIT_REG_MEM`) | **appelants exclusivement WMV** | 18 ; 18 ; 22 | tout | H |
| `0x921299F0`, `0x9212C918`, `0x9212C9E8`, `0x9212CC68` | diagnostic de blocage GPU (`The GPU is hung!`) | à remplacer par rien | 0-1 | 0 | H |
| `0x92132140`, `0x921323C0`, `0x9212CFC0`, `0x92131BF8`, `0x921337F8` | scaler, EDRAM, modes vidéo (`XGetVideoMode`, `D3DDevice_GetDisplayMode`) | | 1-4 | 0 | H |

Les 110 points d'entrée au total se répartissent ainsi (sortie de `analyse_d3d.py api`) : une vingtaine de
fonctions « à fort trafic » (> 20 sites) listées ci-dessus, une cinquantaine (estimation) de petits *setters*
(états de rendu, viewport, scissor, clear, stencil) à 1-10 sites, le reste étant de l'initialisation/diagnostic.
Les petits *setters* non nommés ici se traitent en bloc : chacun écrit des bits dans l'état du périphérique,
la correspondance est mécanique une fois la structure du périphérique (≈ 14 000 octets) cartographiée.

### 2.3 Comment XUI appelle D3D

Il n'y a **aucune chaîne « Xui » nommant le moteur**, mais le moteur XUI est lié dans la même image et appelle
D3D par des **appels directs** (`bl`), sans table de pointeurs : le périphérique `D3DDevice*` est passé en `r3`.
Les appelants du dessin commun `0x921350A0` hors WMV se regroupent en trois familles : le moteur d'effets d'images
(`0x92185E88`, `0x92188328`, qui contient aussi un générateur de **texte HLSL** : voir §6), le rendu XUI proprement
dit (famille `0x921EA2A0 ... 0x921EB820`, 6 fonctions) et un second groupe (`0x922B1BB0 ... 0x922BAAC8`,
≈ 11 fonctions : liste, texte, vignettes). La trame est classique : `SetTexture` x N, `SetPixelShader`/
`SetVertexShader`, constantes (matrice `WorldViewProj`, `ColorFactor`, `SolidColor`...), `SetStreamSource`,
un dessin. L'ensemble tient dans les ≈ 25 fonctions « à fort trafic » ci-dessus.

Point d'attention : **XAM 17559 embarque sa propre copie complète de D3D** (chaînes `D3D` et `GPU is hung`
dans `xam17559.pe`, 34 shaders propres). Avec XAM recompilé et chargé comme module, il y aura **deux instances
de la bibliothèque D3D dans le même processus**. Le remplacement natif doit donc se faire **par motif de code**
(signatures d'instructions des ≈ 35 fonctions), pas par adresse fixe, et partager un seul backend hôte.

## 3. Ce qui reste à découvrir (non couvert par cette étude)

- Les ≈ 50 petits *setters* d'états de rendu : à nommer par `SetRenderState_*` (valeurs `D3DRS_*` -> bits de
  l'état Xenos). Mécanique mais fastidieux.
- `BeginTiling/EndTiling` : **non identifié** (pas de fonction avec motif `SET_BIN_*`/`SET_BIN_MASK` dans la
  bibliothèque ; les opcodes `0x4C-0x4F` n'apparaissent pas). Cohérent avec un rendu 720p sans MSAA, qui tient dans
  les 10 Mo d'EDRAM : **pas de tiling attendu dans le Dashboard 6770**. Hypothèse à confirmer en lisant
  `D3DPRESENT_PARAMETERS.MultiSampleType` à l'initialisation.
- `Clear` : probablement via `Resolve` (clear à la résolution) et un petit dessin plein cadre ; à confirmer.

## 4. Shaders du Dashboard 6770

Recherche : fanion `0x102A1100` (pixel shader) / `0x102A1101` (vertex shader) suivi d'un en-tête cohérent
(`virtualSize`, `physicalSize`, `constantTableOffset`, `shaderOffset` : disposition d'après XenosRecomp). Résultat
sur `work/dash.pe` (commande `analyse_d3d.py ... shaders`) :

| | Nombre |
|---|---|
| Conteneurs de shaders (tous dans `.rdata`, aucun dans `.data`/`.embsec_*`) | **221** (217 distincts à l'octet près) |
| Vertex shaders / pixel shaders | **82 / 139** |
| Versions déclarées dans la table de constantes | `ps_3_0` : 106, `vs_3_0` : 80, sans table : 35 |
| Microcode Xenos total | **165 336 octets** (≈ 13 800 instructions) ; conteneurs : 259 548 octets |
| Famille « décodeur vidéo WMV » (constantes `cYUV`, `cDim`, `inResiduals`, `outTiles`, `g_ImageSampler`...) | **151** (64 VS, 87 PS), ucode moyen 1,4 Ko (VS) / 0,8 Ko (PS), maximum 3,7 Ko |
| Famille « UI / effets / divers » | **70** (18 VS, 52 PS), ucode moyen 133-154 octets, max 444 octets |
| VS à exports mémoire (memexport) probables | **64**, tous dans la famille vidéo (constantes `out*`) |
| Autres emplacements (`shrdres.xzp` = 852 entrées : PNG, `.xus`, `.xma`; `XenonCLatin.xtt`...) | **0 shader** |

Les autres modules `.xex` du dossier `flash/` (hud, huduiskin, signin, marketplace...) sont compressés ; leur
contenu n'a pas été examiné (pas d'outil d'extraction construit dans cette session : `tools/xexinfo/build` est
vide). **À vérifier** : ils ne contiennent a priori pas de shaders propres (les modules XUI s'appuient sur le moteur du
`dash.xex`), mais ce n'est pas démontré.

Contenu de la famille UI, par les noms de constantes : `SolidColor`, `ColorFactor`, `TextureSampler`,
`ColorConvertMatrix`, `ShadowColor`, `WorldViewProj`, `InvViewport`, `LocalToTexTransform`, `UVTrans`,
`Diffuse`, `MainTexture`, `PrevTexture`, `Threshold`, `LineThickness`, `VideoTexture`, `FilterMatrix`,
`AlphaValue`. Les 10 pixel shaders à `ColorFactor` et les 2 VS à `WorldViewProj` en tête de `.rdata`
(`0x9200C038...0x9200D2E8`) sont le noyau XUI (couleur unie, texture, texture teintée, ombre portée, conversion
de couleur). ≈ 30 PS sans table de constantes (`0x920C7718...0x920CC898`) sont des variantes d'effets
(taille ucode 0x48-0x1BC).

Format du conteneur (entiers big-endian) : `flags` (= magic `0x102A110x`, bit 0 = vertex), `virtualSize`,
`physicalSize`, `field0C`, `constantTableOffset` (table de constantes D3DX : CTAB `vs_3_0`/`ps_3_0`, version du
compilateur `2.0.5526.0`), `definitionTableOffset` (constantes flottantes/entières par défaut), `shaderOffset`
(-> `physicalOffset`, `size`, interpolateurs, éléments de vertex declaration). Le **microcode** se trouve à
`container + virtualSize + physicalOffset`. Le flot de contrôle suit le format Xenos (mots de 48 bits par paires :
1975 `exec`, 221 `exec_end`, 267 `cond_exec_predicated`, 418 `alloc`) ; **aucune boucle ni appel** dans le flot de
contrôle (opcodes absents), ce qui simplifie la traduction.

Conséquence : une traduction **hors ligne** (XenosRecomp-like) de ces 221 shaders est possible dès maintenant ;
la clé de recherche à l'exécution est un hachage du contenu du conteneur ou du microcode, ce qui évite d'avoir à
connaître l'adresse du shader.

## 5. Sources publiques (lecture web seule)

### XenosRecomp (hedge-dev) — licence **MIT**

- Outil C++17 / CMake : convertit des shaders Xenos en **HLSL**, puis DXC (DirectX Shader Compiler) produit DXIL
  et SPIR-V. Fichiers : `shader_recompiler.cpp` (1888 lignes), `shader.h` (structures du conteneur : `flags`,
  `virtualSize`, `physicalSize`, `constantTableOffset`, `definitionTableOffset`, `shaderOffset`, interpolateurs,
  éléments de vertex), `constant_table.h`, `dxc_compiler.cpp`, `shader_code.h`, `main.cpp`.
- Mode répertoire : scanne des binaires de shaders et génère un cache SPIR-V (compressé smol-v puis zstd)
  exporté en `.cpp`, clé = hachage XXH3 64 bits.
- **Réutilisable** : l'analyse du conteneur, le décodage ALU/fetch/flot de contrôle, la génération HLSL
  (registres `r#`, `c#`, prédicat `p0`, registre d'adresse `a0`, sorties `oPos`/`oC0-3`/`oDepth`), la
  mécanique cache/hachage. Compatible avec notre cas (shaders `vs_3_0`/`ps_3_0` standards).
- **Spécifique à Sonic Unleashed** (blocs `UNLEASHED_RECOMP`) : Z inverse (reverse-Z), `g_SwappedTexcoords`,
  instanciation par second flux de vertex, sémantiques forcées en `uint4`, emplacements de sortie Vulkan codés en
  dur (`USAGE_LOCATIONS`), alpha-test, `R11G11B10`. Le dépôt dit lui-même « n'attendez pas que ça marche tout
  seul » et liste comme **non implémentés** : indexation dynamique de registres, **memexport**, taille de point,
  constantes entières, la plupart des options de *fetch* de texture. Seuls `TextureFetch` et
  `GetTextureWeights` sont gérés, et aucune boucle/appel dans le flot de contrôle (ce qui nous convient).
- Conclusion : base de départ utilisable **après adaptation** (retirer les hacks, régler les emplacements
  d'interpolateurs). Comme l'outil est MIT, on peut le **vendoriser** (licence et notice conservées), mais il faudra
  le compiler : DXC ajoute une dépendance lourde. Alternative : écrire un traducteur Xenos -> SPIR-V ou GLSL en
  Python/C++ propre (70 shaders UI de moins de 450 octets : faisable ; les 151 shaders vidéo ne le sont pas
  sans memexport).

### UnleashedRecomp (hedge-dev) — licence **GPL-3.0** (`COPYING`)

- Portage de Sonic Unleashed ; couche de rendu `UnleashedRecomp/gpu/video.cpp` (7 882 lignes, 285 Ko) au-dessus de
  **plume** (RHI Vulkan/D3D12/Metal, voir plus bas). Lecture partielle seulement (le fichier est tronqué par
  l'outil de lecture ; je n'ai pas pu lire les lignes des *hooks* eux-mêmes).
- Architecture observée : les appels D3D du jeu sont **mis en file** sous forme de `RenderCommand`
  (`SetRenderState`, `DrawPrimitive`, `DrawIndexedPrimitive`, `DrawPrimitiveUP`, `StretchRect`, `Clear`,
  `SetTexture`...) consommés par un fil de rendu ; états en `PipelineState` -> `g_pipelines` (carte indexée par
  hachage) avec **précompilation asynchrone** (cache d'états sur disque) ; textures et tampons gérés par
  allocateurs d'upload avec byte-swap ; `StretchRect` et MSAA gérés par `g_pendingSurfaceCopies` /
  `g_pendingMsaaResolves` (shaders de résolution MSAA 2x/4x/8x) ; shaders intégrés (copie, flou, gamma, ImGui...).
- **Réutilisable (en idée)** : la structure (file de commandes -> fil de rendu -> pipelines hachés + cache
  asynchrone). **Pas réutilisable en code** : GPL-3.0 est copyleft fort ; copier du code obligerait à placer
  notre travail sous GPL-3.0. Tout y est aussi spécifique : liste de hooks de la fonction par adresse du jeu,
  modèle de shaders du moteur Hedgehog, correctifs de cadence, installateur, interface.
- **Remarque d'architecture** : le modèle est exactement celui que nous envisageons, ce qui valide l'approche.
  Il faudra l'écrire nous-mêmes, d'après la description ci-dessus et les spécifications publiques D3D9/Xenos.

### plume (renderbag) — licence **MIT**

- RHI « plus petit dénominateur commun » D3D12/Vulkan/Metal, issue de RT64, non déclarée prête pour la production
  (gestion des barrières et transitions encore à affiner). Réutilisable (MIT) mais elle n'apporte qu'une
  commodité : Vulkan direct reste raisonnable pour un seul backend Linux. Décision : **ne pas l'adopter** au
  départ (dépendance et surcouche inutiles) ; réévaluer si un backend D3D12/Metal devient souhaitable.

## 6. Architecture proposée

```
code recompilé (dash.xex / xam.xex)
   |  appels D3D9 (≈ 35 fonctions interceptées par motif de code : REX_HOOK)
   v
common/gpu_d3d/ (nouveau)
   - état du périphérique hôte (rendertargets, textures, flux, shaders, constantes, états)
   - table des ressources : D3DResource invité <-> objet hôte (adresse invitée = clé)
   - file de commandes + fil de rendu (modèle UnleashedRecomp)
   v
backend Vulkan (nouveau, sans plume)
   - pipelines hachés + cache disque
   - textures : conversion des formats Xenos + détuilage à l'upload
   - shaders : cache SPIR-V généré hors ligne ; recherche par hachage du conteneur
```

Principes :

1. **Interception par motif** (signatures des fonctions du §2.2) pour rester indépendant de l'adresse, donc
   réutilisable sur 13604/17559 et sur la copie de D3D dans XAM. Les `D3DResource` restent en mémoire invitée
   (le code invité les lit : compteur de références, en-têtes de texture) ; un objet hôte est associé à l'adresse.
2. **Shaders hors ligne** : outil Python/C++ qui parcourt `.rdata` à la recherche de `0x102A110x`, produit un cache
   (SPIR-V) indexé par XXH3/SHA du conteneur. À l'exécution : `SetPixelShader(ptr)` -> hachage -> pipeline.
   Pour les 64 VS memexport : **ne pas les traduire**, remplacer le décodage WMV (voir risques).
3. **Pas de GPU « émulé »** : le fil de rendu ne voit jamais de paquet PM4 ; `VdSwap`, anneau et fences deviennent
   des crochets (no-op ou signal de fin de trame).
4. **Oracle de test** : le plugin `xenos` actuel (PM4 -> Vulkan) reste disponible pendant la transition
   pour **comparer image par image** les captures ; il est retiré une fois la parité atteinte.
5. Les chaînes « HLSL texte » du moteur d'effets (`0x92186330` : *Edge Detection*, *Gaussian Blur*, *Texture
   Sample*, structure `VSOUT`) montrent un générateur de source de shader. Son résultat n'est consommé nulle part
   dans la bibliothèque D3D liée (aucun compilateur HLSL repéré) : probablement code de test/inutilisé,
   **à vérifier à l'exécution** ; sinon il faudrait un compilateur à la volée (non prévu).

## 7. Risques

| Risque | Gravité | Détail / parade |
|---|---|---|
| **Memexport du décodeur WMV** (64 VS) | élevée | XenosRecomp ne le gère pas ; implémentation = tampons de stockage + sémantique d'écriture des flux. Parade recommandée : intercepter le décodeur vidéo (`0x92540000...`, ≈ 1 Mo de code) et le remplacer par du décodage logiciel (ffmpeg). Perte : aucune pour l'interface (bandes-annonces), coût : le contrat du décodeur. |
| **Resolve / EDRAM** | moyenne | Seulement 2 appelants de `Resolve` et pas de tiling détecté : un EDRAM « virtuel » (rendertarget Vulkan + copie vers texture) suffit. À confirmer : formats `k_2_10_10_10`, `k_16_16_16_16_FLOAT`, fonds clairs « clear at resolve ». |
| **Formats et détuilage des textures** | moyenne | Textures Xenos en *tiled* (swizzle 32x32) + endian-swap ; formats DXT1/3/5, `8_8_8_8`, `5_6_5`, `k_8`, cubes/3D. Les textures de l'interface proviennent surtout de `shrdres.xzp` (PNG) et des scènes XUI : le décodage PNG/DDS est fait côté invité, puis uploadé en mémoire GPU ; le détuilage ne concerne donc que ce que le code invité écrit lui-même. |
| **Deux copies de D3D (dash + XAM 17559)** | moyenne | Interception par motif ; backend unique ; ordre d'initialisation (deux `Direct3D_CreateDevice` ?). |
| **Dérive entre versions de D3D** | moyenne | 6770, 13604, 17559 n'ont pas le même D3D (structure du périphérique, offsets). Dérivation automatique par signatures de motif ; coût incompressible par version. |
| **Shaders XUI** | faible | 70 shaders courts, sans boucle ni appel. Les risques sont dans les conventions d'interpolateurs et de constantes (registres `c`/`b`/`i`), pas dans la complexité. |
| **Code D3D propre à XAM (17559)** | moyenne | 34 shaders (29 PS) et une seconde bibliothèque D3D : mêmes techniques, volume modeste. |
| **Hypothèses non vérifiées** | moyenne | Noms de la table du §2.2 (identifications par motif), absence de tiling, absence de memexport hors WMV, absence de shaders dans les modules `flash/*.xex`. |
| **Licences** | faible | XenosRecomp et plume : MIT (vendorisables avec notice). UnleashedRecomp : GPL-3.0, **ne pas copier de code**. Les shaders extraits de `dash.xex` sont des fichiers Microsoft : jamais versionnés (cache généré localement à l'import, comme le reste). |
| **Performance** | faible | Quelques centaines d'appels de dessin par image. |

## 8. Plan d'étapes chiffré (jours-personne, ± 30 %)

### 2.0.6770.0

| Étape | Contenu | Jours |
|---|---|---|
| A1 | Cartographie complète de D3D 6770 : structure du périphérique, les ≈ 50 petits *setters*, signatures de motif, outil de génération de la table d'interception | 5 |
| A2 | Cadre : `REX_HOOK` des ≈ 35 entrées, état hôte, table de ressources, file de commandes, fil de rendu, `Swap` -> présentation, neutralisation des `Vd*` | 7 |
| A3 | Backend Vulkan : rendertargets/backbuffer, états (blend/depth/stencil/cull), flux de vertex et déclarations, tampons d'index, viewport/scissor, `Clear`, dessin, pipelines hachés + cache | 10 |
| A4 | Textures : lecture des *fetch constants*, formats, détuilage + endian-swap à l'upload, mips, cubes/3D, `Lock`/`Unlock` | 8 |
| A5 | Shaders : extracteur de conteneurs, traducteur hors ligne (adaptation XenosRecomp ou traducteur maison) pour les 70 shaders UI/effets, cache par hachage, constantes | 10 |
| A6 | `Resolve` / EDRAM virtuel + copies de surface | 4 |
| A7 | Décodage vidéo : remplacement du décodeur WMV (hook + ffmpeg) | 5 |
| A8 | Mise au point visuelle contre l'oracle (plugin `xenos` actuel), correctifs, retrait de l'émulation | 6 |
| **Total 6770** | | **55** |

### 2.0.13604.0 (NXE)

Réutilise A2-A6 ; coût : nouvelle dérivation de D3D (signatures, offsets), nouveaux effets NXE (flou, avatars non
supportés d'emblée, tuiles), ≈ 230-260 shaders (extrapolation à partir de la 7258 : 246 conteneurs).
Estimation : **+14 jours** (cartographie 3, shaders 5, effets/textures 4, mise au point 2). Le dash 13604 n'est pas
disponible dans `work/` : estimation non mesurée.

### 2.0.17559.0 (Metro)

Mesuré sur `work/port/2.0.17559.0/dash.pe` : **220 conteneurs (33 VS / 187 PS)** ; `xam17559.pe` : **34 (5 VS / 29 PS)**.
Beaucoup plus de PS et peu de VS : l'interface Metro utilise des effets de pixel en nombre ; plus de code
D3D à repérer, deux bibliothèques D3D. Estimation : **+17 jours** (cartographie 4, shaders 6, deux
bibliothèques 3, mise au point 4).

**Total : ≈ 86 jours-personne** (55 + 14 + 17). Chemin critique : A1 -> A2 -> A3 -> A5. Premier jalon visible
utile : **A1-A3 + noyau XUI de A5 (≈ 12 shaders)** = écran de fond et boutons, ≈ 20 jours ; l'émulation actuelle
peut servir en parallèle d'oracle.

## 9. Reproduire l'analyse

```
python3 -I tools/analyse_d3d.py work/dash.pe dashboards/2.0.6770.0/generated/dash shaders
python3 -I tools/analyse_d3d.py work/dash.pe x familles
python3 -I tools/analyse_d3d.py work/dash.pe x cf
python3 -I tools/analyse_d3d.py work/dash.pe x api
python3 -I tools/analyse_d3d.py work/dash.pe x who 921350A0
```

Limites : l'outil ne lit que l'image PE décompressée (jamais versionnée) ; il suppose le format de conteneur décrit
au §4 (non vérifié contre le code de la XDK, seulement recoupé avec la description publique de XenosRecomp et la
cohérence interne : sur le premier conteneur, `virtualSize + physicalSize` tombe exactement sur le suivant, et 221 conteneurs passent le filtre de plausibilité).
