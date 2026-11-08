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
| A5 | Shaders : extracteur de conteneurs (fait), traduction hors ligne par XenosRecomp patché (fait, voir §10), cache par hachage, liaison des constantes/échantillonneurs, spécialisation | 6 *(10 avant l'essai §10)* |
| A6 | `Resolve` / EDRAM virtuel + copies de surface | 4 |
| A7 | Décodage vidéo : remplacement du décodeur WMV (hook + ffmpeg) | 5 |
| A8 | Mise au point visuelle contre l'oracle (plugin `xenos` actuel), correctifs, retrait de l'émulation | 6 |
| **Total 6770** | | **51** *(55 avant l'essai)* |

### 2.0.13604.0 (NXE)

Réutilise A2-A6 ; coût : nouvelle dérivation de D3D (signatures, offsets), nouveaux effets NXE (flou, avatars non
supportés d'emblée, tuiles), ≈ 230-260 shaders (extrapolation à partir de la 7258 : 246 conteneurs).
Estimation : **+12 jours** *(14 avant l'essai §10)* (cartographie 3, shaders 3, effets/textures 4, mise au point 2). Le dash 13604 n'est pas
disponible dans `work/` : estimation non mesurée.

### 2.0.17559.0 (Metro)

Mesuré sur `work/port/2.0.17559.0/dash.pe` : **220 conteneurs (33 VS / 187 PS)** ; `xam17559.pe` : **34 (5 VS / 29 PS)**.
Beaucoup plus de PS et peu de VS : l'interface Metro utilise des effets de pixel en nombre ; plus de code
D3D à repérer, deux bibliothèques D3D. Estimation : **+15 jours** (cartographie 4, shaders 4, deux
bibliothèques 3, mise au point 4) ; shaders ramenés de 6 à 4 jours, soit **+15 jours** au lieu de +17.

**Total : ≈ 78 jours-personne** (51 + 12 + 15 ; 86 avant l'essai du §10). Chemin critique : A1 -> A2 -> A3 -> A5. Premier jalon visible
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

## 10. Essai XenosRecomp (2026-11-06)

XenosRecomp (hedge-dev, MIT) a été cloné avec ses sous-modules dans `work/XenosRecomp` (non versionné), compilé et
appliqué aux 221 conteneurs du `dash.pe` 6770.

**Build : OK**, sans dépendance système manquante (CMake 4, Ninja, clang++ 20+ ; GCC non essayé). Particularités :
(1) `/usr/local/bin/cc` de cette machine est un wrapper qui casse le test du compilateur C de CMake : forcer
`CC=clang CXX=clang++` ; (2) le DXC Linux est fourni **déjà compilé** dans le sous-module (`thirdparty/dxc-bin`,
`libdxcompiler.so` + `dxc-linux`), rien à construire ; (3) le mode DXIL est réservé à Windows, seul SPIR-V est produit ici ;
(4) `-DCMAKE_BUILD_TYPE=Release` définit `NDEBUG` et neutralise les `assert` de l'outil, qui sont sa seule détection
d'entrée inattendue : j'ai compilé avec `-O1` sans `NDEBUG` (`-DCMAKE_CXX_FLAGS_RELEASE="-O1 -g0"`).
`UNLEASHED_RECOMP` n'est **pas** défini : les hacks Sonic (reverse-Z, instanciation « méta », matrices
`g_MtxProjection`...) sont absents de la traduction, rien à neutraliser.

**Entrée : aucun adaptateur de format.** XenosRecomp lit les conteneurs bruts (fanion `0x102A11xx`, champs big-endian) :
`tools/analyse_d3d.py extract <dir>` écrit les 221 conteneurs tels quels (70 `ui/` = 18 VS + 52 PS ; 151 `video/` = 64 VS + 87 PS,
mêmes familles que la commande `familles`). Le mode « fichier unique » de XenosRecomp sort le HLSL ; le pilote
`tools/xenos_batch.py` lance un processus par shader (l'outil s'arrête sur `assert`), puis `dxc-linux -spirv` avec les
options de l'outil (`-fvk-use-dx-layout`, `-fvk-invert-y` pour les VS) et écrit un tableau de résultats.

**Deux adaptations de XenosRecomp** (`patches/xenosrecomp-dash360.patch`, 65 lignes, à appliquer sur le dépôt cloné) :
1. 30 shaders de l'interface (28 PS + 2 VS) **n'ont pas de table de constantes** (`constantTableOffset == 0` : aucune
   constante nommée) ; l'outil les refusait par `assert`. Correctif : table vide.
2. Les registres de constantes et échantillonneurs **non nommés** (`c0`, `s1`...) n'étaient pas déclarés (l'outil suppose
   que le compilateur du jeu a toujours fourni les noms) : DXC échouait (« use of undeclared identifier »). Correctif :
   macros par défaut `cN` -> `PixelShaderConstants/VertexShaderConstants + N*16` et `sN_Texture*/Sampler DescriptorIndex`
   pour tout registre ni nommé ni défini dans la table de définitions (littéraux du compilateur, ex. `c252-c255`).

**Résultats (après les deux correctifs)**

| Famille | Conteneurs | HLSL sans erreur | SPIR-V compilé |
|---|---|---|---|
| Interface, VS | 18 | 18 | 18 |
| Interface, PS | 52 | 52 | 52 |
| **Interface, total** | **70** | **70** | **70 (100 %)** |
| Décodeur WMV, PS | 87 | 87 | 87 |
| Décodeur WMV, VS | 64 | 0 | 0 |
| **WMV, total** | **151** | **87** | **87 (58 %)** |

Sans correctifs : interface 40/70 en HLSL ; avec le seul correctif 1 : 70/70 en HLSL mais 40/70 en SPIR-V (30 échecs DXC, noms non déclarés).

**Échecs : 64 vertex shaders WMV, tous à export mémoire** (les 64 portent des constantes `outTiles/outBuffer/outImage`,
confirmé par `analyse_d3d.py cf`) : 51 échouent au `assert` de `vfetch` (lecture d'un flux de sommets non déclaré : le
VS lit des tampons bruts, pas des éléments de sommet) et 13 à l'`assert` d'interpolateur (sorties d'export mémoire prises
pour des interpolateurs). C'est la limite connue de l'outil (pas de `memexport`) ; elle ne touche aucun shader de l'interface.
Aucune instruction ALU/fetch non gérée n'a été rencontrée sur les 70 + 87 shaders traduits.

**Relecture manuelle** (4 shaders, tous cohérents avec le rôle attendu) :
- `9200C460` (PS) : `oC0 = SolidColor * ColorFactor` : remplissage uni, exact.
- `920CA550` (PS, texte/ icône avec fondu) : `t = tex(s0, uv) ; oC0 = (t - c31) * color0.w + c31` : mélange vers une
  couleur de base selon l'alpha du sommet ; les `r0 = iTexCoord0`, `r1 = iColor0` sont bien initialisés depuis les
  interpolateurs, `cndge` rendu en `select(x >= 0.0, ...)` (sémantique Xenos correcte).
- `920CC898` (PS, flou en 6 prises) : chaîne de pas `uv += pas`, 6 `tfetch2D` sur `s0`, pondération géométrique
  `c1.w * c2.z^k`, bornage par `cndge` : plausible comme flou radial/lueur.
- `9200C038` (VS XUI) : `WorldViewProj` puis `InvViewport`, `LocalToTexTransform`, `UVTrans`, et `c255.x = -0.5`
  (littéral de la table de définitions, la correction de demi-pixel) ; l'outil ajoute `oPos.xy += g_HalfPixelOffset * oPos.w`.
  Cohérent. Le PS `9200C818` (conversion de couleur avec `ColorConvertMatrix`) utilise de même le littéral `c255 = (0,1,0,0)`.

**Réserves**
- Justesse **jugée sur lecture**, non vérifiée à l'image (aucune comparaison pixel avec l'émulation actuelle).
- Quirk bénin : pour le dernier tableau nommé, l'outil déclare `registerCount` jusqu'à 256 (`select(INDEX < 256, ...)`) ;
  sans conséquence tant que le moteur ne lit pas hors tableau.
- Liaison à faire côté exécution : le modèle de l'outil (constantes lues par `RawBufferLoad` dans une zone de pointeurs
  poussés, descripteurs de textures indexés par registre, constantes d'état dans `SharedConstants`) est celui d'UnleashedRecomp ;
  notre backend (A3-A5) doit reproduire cette disposition (`g_Booleans`, `g_HalfPixelOffset`, `g_AlphaThreshold`, spécialisation
  alpha-test) ou la remplacer.
- Les 87 PS vidéo compilent mais ne servent à rien sans les VS : la vidéo reste à traiter par remplacement du décodeur (A7).

**Impact sur l'estimation** : la traduction des 70 shaders d'interface est **acquise** (aucun traducteur maison). A5
passe de 10 à 6 jours (reste : cache par hachage, liaison des constantes et échantillonneurs, tests visuels), et les lignes
« shaders » de 13604 et 17559 baissent de 2 jours chacune. Total **≈ 78 jours-personne** (contre 86). Le risque
« memexport WMV » est inchangé (remplacement du décodeur conservé en A7).

**Reproduire**
```
git clone --recursive https://github.com/hedge-dev/XenosRecomp work/XenosRecomp
git -C work/XenosRecomp apply "$PWD/patches/xenosrecomp-dash360.patch"
(cd work/XenosRecomp && CC=clang CXX=clang++ cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
   -DCMAKE_CXX_FLAGS_RELEASE="-O1 -g0" && cmake --build build)
python3 -I tools/analyse_d3d.py work/dash.pe work/port extract work/shaders6770
python3 -I tools/xenos_batch.py work/shaders6770 work/shaders_out
```
Les dossiers `work/shaders6770` (microcode Microsoft) et `work/shaders_out` (HLSL/SPIR-V dérivés) ne sont jamais versionnés.

## 11. Avancement : premier jalon du rendu natif sur 6770 (2026-11-06, `native_gpu/`)

Le plugin `xenos` (PM4 → Vulkan) est **retiré de la boucle** : avec `-DDASH_NATIVE_GPU=ON` (option CMake de
`dashboards/2.0.6770.0`, désactivée par défaut), le Dashboard 6770 démarre **sans plugin GPU** (`app.h` vide
`config.gpu_plugin` : le SDK passe en mode « détaché », fenêtre SDL3 seule, aucune modification du SDK nécessaire) et
affiche l'écran Games **par du code natif** : les fonctions D3D de la XDK sont remplacées par des fonctions hôte, les
shaders sont les SPIR-V de XenosRecomp. Aucun paquet PM4 n'est lu.

### 11.1 Architecture (pas de code d'UnleashedRecomp ; MIT/BSD seulement)

| Fichier (`native_gpu/src/`) | Rôle |
|---|---|
| `d3d_trace.cpp` + `d3d6770_entries.inc` | Un enveloppeur par fonction listée (3 281 adresses : bibliothèque D3D `0x92120000-0x92140000`, moteur d'effets/XUI, `0x925CA000-0x925D0000`) : **remplace l'alias faible `sub_XXXXXXXX`** du code généré (symbole fort, `__imp__sub_XXXXXXXX` = original), compte/journalise (traçage), puis appelle le crochet `ngpu_hook_XXXXXXXX` s'il existe. C'est le mécanisme « REX_HOOK » adapté aux fonctions du jeu (`common/` ne remplace que des imports `__imp__*`). |
| `hooks_frame.cpp`, `null_gpu.cpp` | Cycle de vie : **GPU nul** (le D3D invité scrute un bloc de retour écrit par le GPU : un fil écrit « tout consommé » dans `*(dev+10896)` ; attentes `92129788`/`921296F0` satisfaites ; **fil vblank** 60 Hz qui appelle le gestionnaire invité `92121210(dev)`), `Clear`, `Swap` → présentation. |
| `hooks_draw.cpp` | État D3D suivi côté hôte (shaders, textures, constantes VS/PS, déclaration de sommets) et dessin (`DrawVerticesUP`, `BeginVertices`+`EndVertices`). Conversion des sommets BE → float4, expansion `RECTLIST`/`QUADLIST`, blend/masque lus dans les registres cachés du périphérique. |
| `guest_tex.cpp` | Textures Xenos → RGBA8 : lecture de la *fetch constant*, endian, **détuilage 2D**, DXT1/3/5, 8888, 8, 8_8, 565, 1555, swizzle ; copie protégée (`process_vm_readv`). |
| `vk_backend.cpp` | Vulkan 1.3 direct (libvulkan, **SDL + Vulkan sans plugin** : surface X11/Wayland créée depuis l'`SDL_Window` du SDK) : tampon d'image 1280x720, rendu dynamique, cache de pipelines (hachage vs/ps/topologie/attributs/blend), anneau de constantes (adresse de tampon, modèle `PushConstants` de XenosRecomp : 3 adresses 64 bits), descripteurs « bindless » (tableaux 2D/échantillonneurs, ensembles 0 et 3), présentation par blit. |

Les SPIR-V sont lus à l'exécution dans `DASH_NGPU_SHADERS` (défaut `work/shaders_out`), nom `ui_<adresse du conteneur>_VS|PS.spv`
(adresse du conteneur dans `.rdata`, obtenue en interceptant `CreateVertexShader/PixelShader`). Rien de Microsoft n'est versionné.

### 11.2 Cartographie D3D 6770 confirmée à l'exécution (et écarts avec l'étude §2.2)

Méthode : enveloppeurs de comptage (`DASH_D3D_TRACE`, `DASH_D3D_SEQ=<image>`, recherches de pointeurs `DASH_D3D_FINDSHADER/FINDDECL`),
arguments relevés, puis vérification par le rendu. Instantané en idle sur l'écran Games : ~60-160 dessins/image.

| Fonction | Étude (§2.2) | **Constaté** |
|---|---|---|
| `0x921350A0` | « dessin commun » (31 sites) | **faux** : appelée **1 fois par image**, par `Swap` (`92121EE8` → `921350A0` → `92121950`/`VdSwap`) ; ce n'est pas le dessin. |
| **dessin** | — | `0x925CCB68` = `DrawVerticesUP(dev, prim, n, données, stride)` (26 sites : famille XUI `921EA2A0…921EB820`, effets `92188590`…), `0x925CC6B8` = `BeginVertices(dev, prim, n, stride)` → pointeur d'écriture, `0x925CCB58` = `EndVertices`, `0x925CCBB0` = `DrawIndexedVerticesUP` (via `925CD0E0`). **La bibliothèque D3D déborde donc de `0x92120000-0x92140000`** (jusqu'à `~0x925D0000`, code de la XDK compilé avec le décodeur WMV). Primitives vues : 5 (fan), 8 (rect list), 13 (quad list). |
| `0x9212E4E8` / `0x9212E7E8` | SetVertexShader / SetPixelShader | **inversés** : `9212E7E8` = `SetVertexShader(dev, obj)`, `9212E4E8` = `SetPixelShader(dev, obj)`. |
| `0x9212EE10` / `0x9212ED00` | « verrous ou Begin/End » (F) | `CreateVertexShader(pFunction)` / `CreatePixelShader(pFunction)` (argument = conteneur `102A110x` de `.rdata`, retour = objet ; l'objet pixel contient une copie du conteneur à +40). |
| `0x9212E2D8` / `0x9212E3C0` | Set*ShaderConstantF (M) | confirmé : `(dev, registre, pDonnées BE, nb float4)`, VS puis PS (ex. `WorldViewProj` c0-c3, `InvViewport` c4-c7 ; `ColorFactor` c1 PS). |
| `0x92123F20` | SetTexture | confirmé `(dev, étage, D3DBaseTexture*)`. En-tête de texture : `D3DResource` (24 o) + `MipFlush` + **fetch constant de 6 mots à +28** (= `xe_gpu_texture_fetch_t` du SDK) ; les ressources de dégradé de l'interface sont des textures **128x1** (gradients XUI), les polices des 256x256 `k_8`. |
| `0x92128AF0` | (non nommée) | `D3DDevice_Clear(dev, nbRects, rects, flags, couleur ARGB, z, stencil)` → `921289C8` → quad plein cadre `92127880`. Ordre par image : **Clear → dessins → Swap**. `Resolve` (`92128D20`) **non appelée** dans l'écran Games au repos. |
| `0x9212EB48` (`9212EA58`) / `0x9212EA00` | — | `CreateVertexDeclaration(pÉléments)` / `SetVertexDeclaration(dev, decl)`. Élément de 12 octets : `{u16 flux, u16 décalage, u32 type (format GPU = 6 bits bas : 0x25 = 32_32_FLOAT, 0x26 = 32x4, 0x39 = 32x3, 0x06 = 8888), u8 méthode, u8 usage, u8 indice}`. **Le format n'est pas dans le microcode du VS** (instruction `vfetch` à format 0) : D3D le patche d'après la déclaration, on le lit donc dans la déclaration. |
| `0x92138C50`, `92138FF0`, `92139D88`… | — | émetteurs PM4 de groupes de registres depuis le **cache de registres du périphérique** (`dev+10368` = base 0x2000, `+10444` = 0x2100, `+10528` = 0x2180, `+10548` = 0x2200 [RB_BLENDCONTROL0 = `+10552`], `+10460` = RB_COLOR_MASK) ; on lit blend/masque directement dans ce cache (code natif lu, jamais émis). |
| Fences/GPU | `9212A4E0` etc. | `*(dev+10896)` : bloc de retour GPU (+0 identifiant = `dev+10908`, +4 second mot, +60 pointeur de lecture d'anneau) ; attentes `9212A418`, `92129788`, `921296F0` ; `92120C98` = scrutation à délai (1,6 M appels/image sous le plugin). Gestionnaire d'interruption enregistré : `921299F0` (lit un registre MMIO) → appelle `92121210(dev)` (vblank : compteur `dev+16532`, file de Swap). |
| Périphérique | — | une seule instance, `0x40001780` (tas), ~21 Ko ; `Direct3D_CreateDevice` non remplacée (l'original tourne, grâce au GPU nul). |

Inchangé/non revérifié : `Release/AddRef` (`9212B950/B8D8`), `SetSamplerState` (`92125D18/92125EA8`, arguments relevés, sémantique non décodée),
`SetRenderTarget` (`92126708`, `921240F0`), `Resolve`, `SetStreamSource` (`92126858`) : **non utilisés** par les dessins `UP` de l'écran Games.

### 11.3 Résultat

- Paliers livrés (commits `native_gpu`) : (1) traçage et cartographie ; (2) runtime sans plugin, GPU nul + vblank, `Clear` + `Swap` Vulkan directs ;
  (3) dessin : pipelines hachés, constantes, sommets, textures détuilées, blend.
- **Écran Games rendu nativement** (fond, lames, dégradés, textes, icônes, avatar, logo) : captures locales
  `dashboards/2.0.6770.0/runs/preuves/natif_natif.png` (natif) et `natif_emulation.png` (plugin `xenos`, même scène) — `runs/` n'est plus versionné.
  Le rendu est **reconnaissable mais incorrect** : couches mal placées/dupliquées, texte rogné, bandes de dégradé.
- Cadence (lavapipe, 1280x720, Xvfb, présentation immédiate) : **natif ≈ 50 images/s** (swaps 300→1700 en 28,2 s) contre **≈ 12 images/s** avec le
  plugin `xenos` (swaps 300→400 en 8,1 s). Ce n'est pas une mesure de GPU réel (le rendu natif omet une partie du travail).
- Reproduire : `cmake -S dashboards/2.0.6770.0 -B dashboards/2.0.6770.0/out/build/linux-amd64-relwithdebinfo -DDASH_NATIVE_GPU=ON && cmake --build … -j12`, puis
  `native_gpu/tools/run_headless_native.sh 92 35 natif_natif` (et `DASH_NGPU_DISABLE=1 … --gpu_plugin=xenos` pour la référence).
  Remettre `-DDASH_NATIVE_GPU=OFF` pour retrouver le binaire d'origine (identique : aucun fichier compilé ne change quand l'option est OFF).

### 11.4 Écarts connus / reste à faire pour la parité (par ordre d'importance)

1. **Cibles de rendu hors écran, `SetRenderTarget`/`Resolve`/EDRAM** : tous les dessins vont dans un seul tampon d'image ; or l'interface compose des passes
   (flou/lueur, vignettes) dans des textures. C'est la cause principale des artefacts (étape A6, non commencée).
2. Viewport/scissor (XUI rogne les listes), profondeur/stencil, cull, états d'échantillonnage (`SetSamplerState` : filtre linéaire/bord par défaut), mips, cubes/3D.
3. Autres chemins de dessin : `DrawIndexedVerticesUP`, `DrawVertices*`/`SetStreamSource` + `SetIndices`, vidéo (A7, memexport).
4. Formats de sommet rares (10_11_11, 2_10_10_10, 16 bits normalisés), `Lock/Unlock` des textures (détection de modification par hachage une fois par image).
5. Pas de recouvrement ImGui en mode détaché (écrans XAM de remplacement `dash_xam_ui.cpp`, lanceur F5 : invisibles tant qu'il n'y a pas de `ImmediateDrawer` hôte) ;
   le curseur/les entrées clavier passent par la fenêtre du SDK (inchangé).
6. Le modèle par adresse fixe vaut pour 6770 seulement ; la dérivation par motif de code (13604/17559, XAM) reste à faire (étape A1 bis).

### 11.5 Estimation révisée

Le jalon « A1-A3 + noyau XUI » (≈ 20 j dans l'étude) est atteint en une session grâce à trois découvertes : le chemin de dessin passe par `DrawVerticesUP`
(sommets en argument, pas de flux/indices à reconstruire), XenosRecomp fournit directement des SPIR-V utilisables, et le SDK sait fonctionner sans plugin.
Reste pour 6770 : RT/Resolve 5 j, états (viewport/scissor/profondeur/échantillonneurs/mips) 4 j, autres chemins de dessin et formats 4 j, vidéo (A7) 5 j,
mise au point contre l'oracle 6 j, soit **≈ 24 j** (contre 51 j au §8) ; 13604 (+10 j) et 17559 (+13 j) inchangés en ordre de grandeur. **Total ≈ 65 j-personne** (± 30 %).

## 12. Jalon 2 (arrêté en cours de route, 2026-11-08) : parité de l'écran Games

Le jalon 2 a été **interrompu volontairement** : l'utilisateur veut réutiliser à terme la couche D3D native de Skate1Native
(`../Skate1Recompiled`, `docs/native_d3d.md`) comme rendu commun (voir **P8** dans `PLAN.md`). `native_gpu/` reste donc un prototype 6770 ;
les enseignements ci-dessous sont à reporter dans la couche commune.

### 12.1 Corrigé (commit de cette session)
- **Cause principale des « couches dupliquées/mal placées » : adresse des textures.** La fetch constant contient une page *virtuelle* avec bits de région
  (A/C/E). Pour la région E (`>= 0xE0000000`, pages de 4 Ko) le SDK décale la mémoire de **+0x1000** ; il faut lire `physical_membase + (base & 0x1FFFFFFF) + 0x1000`
  (`PhysAddr()` dans `guest_tex.h`). Avant : on lisait la page voisine, donc les dégradés 128x1 d'un autre dessin. Après correction l'écran Games est
  presque reconnaissable à l'identique (`runs/preuves/natif2_natif.png` contre `natif2_emulation.png`, non versionnées).
- **`DrawIndexedVerticesUP`** (`925CD0E0(dev, prim, minVertex, nbSommets, nbIndices, pIndices, format, pSommets, stride@r1+84)`) implémenté : c'est lui qui dessine
  les lames/onglets (maillages) ; absent avant, d'où des lames manquantes. Le chemin `DrawVerticesUP` ne portait que les remplissages en dégradé.
- Cache de textures : plus de raccourci « une vérification par image » (le contenu change au sein d'une image).
- Registres d'état lus dans le cache du périphérique (vérifié) : `0x2100+i` à `dev+10444+4i` (masque `0x2104`, alpha ref `0x210E`, viewport `0x210F-0x2114`),
  `0x2200+i` à `dev+10548+4i` (profondeur `0x2200`, blend `0x2201`, couleur `0x2202`, `0x2205` cull, `0x2206` VTE, `0x2208` mode), surface `dev+10368`, scissor de fenêtre `dev+10436/10440`.
  Sur l'écran Games, toutes les valeurs sont constantes (plein écran 1280x720, profondeur désactivée, pas de cible hors écran) : **viewport/scissor, profondeur et
  `Resolve` ne sont pas la cause des artefacts** ; l'hypothèse de l'étape 11.4 (1-2) est donc réfutée pour cet écran.
- Alpha test : tous les PS XenosRecomp portent la constante de spécialisation `1<<1` (`clip(a - g_AlphaThreshold)`, seuil à `SharedConstants+272`) ; non branchée (RB_COLORCONTROL = `0x87000007`, test désactivé sur Games).

### 12.2 Outils ajoutés
- `tools/framecmp.py` : PSNR, écart moyen, grille de zones, planche contact, conversion PPM/XWD/PNG (bibliothèque standard seule). `--noise` pour le plancher de bruit de l'oracle.
- **Mode observation** `DASH_NGPU_OBSERVE=N` (+ `--gpu_plugin=xenos`) : l'oracle dessine, mais les dessins UP/indexés au-delà du N-ième de chaque image sont supprimés ;
  `DASH_NGPU_UPMAX=N` fait de même en natif. Comparer les deux pour N croissant localise le premier dessin divergent (c'est ainsi qu'a été trouvé le décalage +0x1000).
- `DASH_NGPU_DUMP=<image>:<fichier.ppm>`, `DASH_NGPU_PIXLOG=<image>:x,y`, `DASH_NGPU_DRAWINFO=<image>`, `DASH_NGPU_DRAWRANGE=a:b`, `DASH_NGPU_TEXPIX`, `DASH_NGPU_REGDUMP`.
- `native_gpu/tools/run_headless_native.sh` accepte `DASH_BIN` ; build natif dans `dashboards/2.0.6770.0/out/build/native` (copier `librexgpu-xenos.so` pour l'oracle).
- Option `DASH_NATIVE_GPU` ajoutée au gabarit `tools/templates/dashboard/CMakeLists.txt` (OFF).

### 12.3 Parité mesurée
Le fond est animé : une comparaison pixel à pixel entre deux instants est dominée par la phase de l'animation. Mesure brute natif/oracle sur Games
(instants différents) : **écart moyen 14,4/255, PSNR 22,7 dB, 4,2 % de pixels à écart > 32** (avant : 80,1/255, 8,45 dB, 82,6 %) ;
zones entre 6 et 13 d'écart moyen, uniformes (animation du fond). Cette mesure reste **non comparable au plancher de bruit de l'oracle** (non mesuré proprement : il faudrait figer l'horloge invitée). Parité visuelle : fond, panneau, icônes, logo, texte corrects.
FPS non remesuré (≈ 22 images/s sous lavapipe avec la machine chargée ; 50 au jalon 1).

### 12.4 Reste (non fait)
Artefacts résiduels : fines lignes verticales pleine hauteur (x≈235 et 1040), arcs fantômes près des lames, texte légèrement rogné ; cause non identifiée (candidats : rect lists, `DrawVertices`/`SetStreamSource`, échantillonnage mip/bord).
Non fait : cibles hors écran/`Resolve` (non utilisés sur Games), couche ImGui Vulkan, navigation Achievements/Played Games/dialogues en natif, formats rares, comparaison figée.
