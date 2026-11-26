# Rendu natif : `ngpu_core`, façades D3D, shaders

Rendu **natif au niveau D3D** : pas d'émulation du GPU, aucun paquet PM4 lu. Les fonctions D3D de la XDK (bibliothèque liée statiquement dans le XEX) sont remplacées par du code hôte
qui alimente un noyau Vulkan commun, `ngpu_core`. Le plugin `xenos` (PM4 vers Vulkan) ne sert plus que d'**oracle** de comparaison
(`DASH_NGPU_DISABLE=1 --gpu_plugin=xenos` dans un build natif, ou un build `-DDASH_NATIVE_GPU=OFF`). Le Dashboard Kinect 2.0.13604 est la cible principale
(`dashboards/2.0.13604.0-xam`, seule variante Kinect, `DASH_NATIVE_GPU=ON` par défaut). Le 6770 (Blades) et Skate (`games/45410813`) utilisent le même noyau.

## 1. Architecture

```
code recompilé (dash.xex)  --appels D3D-->  d3d_trace.cpp : enveloppeur par fonction (remplace l'alias faible sub_XXXXXXXX ; __imp__sub_XXXXXXXX = original)
   --> crochet ngpu_hook_XXXXXXXX de la façade du profil --> DrawCall / ClearCall / ResolveCall + instantané de l'ombre du périphérique
   --> ngpu_core (Renderer, plume/Vulkan) --> image présentée (A2B10G10R10, une image de retard) --> présentation
XAM (xam.xex, D3D 2099 propre) --> src_xam_d3d/ + hud_front.cpp --> SECONDE instance de ngpu_core --> image 852x480 superposée (HUD)
```

- **Aucun plugin GPU** : `app.h` vide `config.gpu_plugin` (sauf `DASH_NGPU_DISABLE`), le SDK passe en mode détaché (fenêtre SDL seule), sans modification du SDK. `-DDASH_NATIVE_GPU=OFF` n'inclut pas `native_gpu`.
- **Façade par profil** (`NGPU_PROFILE`, 6770 par défaut, fixé à 13604 par le CMakeLists de 13604) : `profiles/dash6770.h`, `dash13604.h`, `hud_xam13604.h` (macros `NGP_*` : adresses et décalages, sans contenu Microsoft).
- **GPU nul** : le D3D invité scrute un bloc de retour écrit par le GPU. Un fil écrit « tout consommé » (`*(dev+fence_ptr)` ; WaitFence/Drain/Second satisfaits) et un **fil vblank 60 Hz** appelle le gestionnaire d'interruption invité.
  `Direct3D_CreateDevice` n'est pas remplacée (l'original tourne grâce au GPU nul). L'attache du GPU nul attend que `*(dev+fence_ptr)` soit non nul.
- **Modèle** : rendu synchrone sous un verrou (le noyau n'est pas thread-safe). L'ombre canonique du périphérique est copiée à chaque appel : le noyau n'a aucun état « D3D ».
- Interception par adresse (profil par version de D3D) ; la dérivation par motif de code (`tools/d3d_match.py`, familles `tools/d3d_signatures/`) sert à retrouver les adresses d'une nouvelle version, à valider ensuite à la main.
- `d3d_trace.cpp` + `d3d6770_entries.inc` (3 281 adresses) / `d3d13604_entries.inc` (1 492 fonctions) : traçage `DASH_D3D_TRACE`, `DASH_D3D_SEQ=<image>`, `DASH_D3D_FINDSHADER/FINDDECL`.

### 1.1 Structure

```
ngpu_core/
  CMakeLists.txt        cible `ngpu_core` (STATIC) + `plume` ; NGPU_BUILD_REPLAY ; renomme les symboles volk (ngpu_vk*)
  THIRD_PARTY.md        origines et licences (tiré de Skate1Native, GPL-3.0 ; plume/volk/VMA MIT, smol-v, xxHash)
  src/  renderer.{h,cpp} (cibles EDRAM, resolves, textures détuilées, tampons, pipelines, descripteurs bindless) ; xenos_decode.h (ombre canonique, fetch constants, formats, détuilage)
        shader_provider.h (ShaderProvider + MapShaderProvider) ; profile.h ; device.{h,cpp} (Core = périphérique plume + Renderer + MapShaderProvider) ; shaders/ (resolve.hlsl, SPIR-V généré par tools/compile_native_shaders.sh)
  profiles/skate1/      profil et façade de Skate1Native EN RÉFÉRENCE (non compilée, sauf profile.h et le cache de shaders)
  third_party/plume     RHI Vulkan seul, volk, VMA, smol-v ; tools/replay (rejeu .sk1c, compile mais jamais exécuté ici) ; tools/d3d (capture.py)
native_gpu/             façades : front_profile.cpp (13604), front6770.cpp (6770, NGPU_CORE=ON), hud_front.cpp (HUD XAM), null_gpu.cpp, hooks_frame.cpp, guest_tex.cpp, write_watcher.h, vk_backend.cpp (présentation SDL)
                        hooks_draw.cpp + vk_backend.cpp = ancien backend 6770 (NGPU_CORE=OFF) ; tools/run_headless_native.sh
```

### 1.2 API du noyau

```cpp
native_gpu::Core core;  core.Initialize(physical, container_lookup, watcher, profile, &error);
core.shaders().AddFile(key, "x.spv");   // ou AddBlob ; clé = XXH3-64 du microcode (XENOS_RECOMP_MICROCODE_KEY)
auto& r = core.renderer();  r.BeginFrame(); r.Clear(ClearCall); r.Draw(DrawCall); r.Resolve(ResolveCall);
r.EndFrame(front_buffer_address);       // r.presented() ; r.WaitIdle() ; SetRenderScale / SetWidth / SetGammaRamp / LoadPipelineCache
```

- **Ombre canonique** (`xenos::DeviceState{const uint8_t* data}`, grand-boutiste, `xenos_decode.h`) : fetch constants `+0x480` (32 x 24 o), constantes de sommets `+0x780`, de pixels `+0x1780`, booléens `+0x2780/+0x2790`,
  registres Xenos par groupes (`+10368` 0x2000, `+10444` 0x2100, `+10528` 0x2180, `+10548` 0x2200, `+10596` 0x2280, `+10680` 0x2300, `+10832` 0x2380).
- `DrawCall` : clés de microcode VS/PS, primitive Xenos (1, 2, 3, 4, 6, 13), comptes, **partie physique du VS telle que D3D l'a patchée** (le noyau y lit les `vfetch`), indices physiques (`indices_address`) ou en ligne (`inline_indices`),
  sommets en ligne (`inline_vertices`), `has_scissor/scissor` (pixels invités ; faux par défaut, sans effet pour 6770 et Skate).
- `Resolve` lit la destination dans `RB_COPY_DEST_BASE/PITCH` (0x2319/0x231A) ; `EndFrame(adresse)` empaquette la texture résolue.
- `ShaderProvider` : SPIR-V non compressé par clé. `MapShaderProvider` (Dashboard : fichiers) et `StaticShaderProvider` (Skate : cache compilé). `WriteWatcher` (`PhysicalWriteWatcher`, pages protégées) ne reconvertit que ce qui change.
- `Profile` : `interface_vertex_shaders`, `corner_vertex_shaders`, `scene_pitch_min/max` (échelle de rendu, image élargie) ; plage vide pour le Dashboard.

## 2. Dashboard Kinect 13604 (`native_gpu/src/front_profile.cpp`)

D3D 2.0.20158 de la XDK. Adresses retrouvées par `d3d_match.py`, validées à la main (`tools/ppc_dis.py`, comparaison avec 5632/6132/6770, graphe d'appels), apprises dans la famille `dash-13604` (`tools/d3d_signatures/references.toml`).
`d3d_match.py analyze` : supported = true, 20 rôles à confiance 1,0. Le test est circulaire pour les adresses ; le rendu (§6) ne l'est pas.

| Rôle | Adresse | Validation / détail |
|---|---|---|
| BeginVertices / EndVertices | `927E0A58` / `927E0A48` | (dev, prim, nb, pas) -> pointeur ; le vidage d'état écrit les groupes de registres ci-dessus, constantes `+1920`/`+6016` |
| DrawVerticesUP | `927E0F18` | enveloppe de BeginVertices (EndVertices fondu : `+13652 -> +48`) |
| DrawIndexedVerticesUP | `927E14A0` | (dev, prim, min, nb, nbIdx, idx, format, sommets, pas à `r1+84`) |
| DrawVertices / DrawIndexedVertices | `927E1540` / `927E1958` | flux du périphérique ; tampon d'indices à `+12612` |
| Clear (ClearF) | `92195E98` | (dev, drapeaux, rect*, couleur* 4 float, f1 = z, **r8 = stencil**) ; appelé par `92195FC0` et BeginTiling |
| BeginTiling | `92196370` | appelle ClearF avec 63 ; non suivi (rendu sans tuiles) |
| Resolve | `921A6908` (enveloppe) / `921A57B8` (interne, 2 arguments de plus) | `RB_COPY_DEST_BASE` `+10780` ; Swap public `9218F138` = Resolve puis `9218EAB0` |
| Swap | `9218EAB0` | appelle `VdSwap` |
| CreateVertexShader / CreatePixelShader | `9219D0F0` / `9219CF08` | objet VS : en-tête 40 o + 832 o d'état vfetch, partie virtuelle à `+872`, **microcode physique (patché par D3D) à `+32`** ; objet PS : virtuelle `+40`, physique `+24` |
| SetVertexShader / SetPixelShader | `9219C8C8` / `9219C6C0` | courants à `dev+12872` / `dev+12868` |
| SetTexture | `92191348` | (dev, étage, texture, drapeaux) ; fetch constants à `dev+(étage+48)*24` = `+0x480` |
| VblankHandler | `9218E290` | appelé par `92196F30` ; compteur `dev+16760` |
| WaitFence / WaitDrain / WaitSecond | `92197358` / `92196CA8` / `92196C10` | `+10896`, `+10908`, mot `+60`, mot `+4` (comme 6770 et Skate) |

- **Périphérique** : canonique pour tout ce que lit le noyau, sauf `vertex_shader 12872`, `pixel_shader 12868`, `index_buffer 12612` (`device_layout.h`, `Canonicalize`). Ce D3D écrit constantes et fetch constants **directement dans le périphérique**
  (`stfs 6016(r31)`, `lwz/stw 1152(r31)`) : aucun crochet `Set*ShaderConstantF`/`SetTexture` n'est nécessaire. Plans de coupe utilisateur à `dev+10272` (6 x float4), hors ombre.
- **Crochets** `NGPU_HOOK(NGP_*)` : GPU nul + vblank, BeginVertices (dessin ouvert, clos au prochain appel D3D suivi car `DrawVerticesUP` fond EndVertices), DrawIndexedVerticesUP, DrawVertices/DrawIndexedVertices, ClearF, Resolve,
  Swap (`EndFrame`, présentation). Le microcode du VS est celui que D3D a patché dans l'objet (lu après l'appel d'origine, copié au BeginVertices) : pas de table de déclarations ; les éléments du quatrième sommet des rect lists
  se déduisent des vfetch. Les objets shader sont lus **dans leur mémoire** au premier dessin (clé XXH3-64 du microcode) : aucun crochet de création indispensable.
- **Plans de coupe utilisateur** : XUI clippe ses textes avec `PA_CL_CLIP_CNTL` (`0x2204`, bits 0-5, `0x0008000F`), valeurs à `dev+10272` ; deux plans opposés (x >= c et x <= c) donnent une fente de largeur nulle. Les SPIR-V de XenosRecomp n'ont pas de
  `ClipDistance` : la façade convertit les plans alignés sur les axes en rectangle de coupe (NDC -> pixels par le viewport), un rectangle vide saute le dessin, les plans obliques sont signalés et ignorés.

## 3. Façade 6770 (`native_gpu/src/front6770.cpp`)

D3D 2.0.5526 ; bibliothèque D3D de `0x92120000` à `~0x925D0000`. Un seul périphérique, `0x40001780` (tas, ~21 Ko). L'ombre du périphérique 6770 est celle de Skate (`BuildShadow` copie les 13 Ko).

| Adresse | Fonction |
|---|---|
| `9212EE10` / `9212ED00` | CreateVertexShader / CreatePixelShader (conteneur `102A110x` de `.rdata`) |
| `9212E7E8` / `9212E4E8` | SetVertexShader / SetPixelShader (**inversés** par rapport à l'intuition) |
| `9212EB48` (`9212EA58`) / `9212EA00` | CreateVertexDeclaration / SetVertexDeclaration ; élément de 12 o `{u16 flux, u16 décalage, u32 type (format GPU, 6 bits bas : 0x25 = 32_32_FLOAT, 0x26 = 32x4, 0x39 = 32x3, 0x06 = 8888), u8 méthode, u8 usage, u8 indice}` |
| `9212E2D8` / `9212E3C0` | Set(Vertex/Pixel)ShaderConstantF (dev, registre, données BE, nb float4) |
| `92123F20` | SetTexture (dev, étage, texture) ; fetch constant de 6 mots à `+28` de l'objet |
| `925CCB68` / `925CD0E0` / `925CC6B8` + `925CCB58` | DrawVerticesUP / DrawIndexedVerticesUP (`925CCBB0`) / BeginVertices + EndVertices ; primitives vues : 5 (fan), 8 (rect list), 13 (quad list), développées en triangles |
| `92128AF0` | Clear (dev, nbRects, rects, flags, ARGB, z, stencil) -> `921289C8` -> quad plein cadre `92127880` ; ordre par image : Clear, dessins, Swap |
| `92121EE8` | Swap : `Resolve` vers un tampon fictif (`0x1F000000`) + `EndFrame` + présentation ; appelle `921350A0` (une fois par image, **pas** le dessin) puis `92121950` -> `VdSwap` |
| `92128D20` | Resolve (non appelée sur l'écran Games au repos) |
| `*(dev+10896)` | bloc de retour GPU (+0 identifiant = `dev+10908`, +4 second mot, +60 pointeur de lecture) ; attentes `9212A418`, `92129788`, `921296F0` ; `92121210(dev)` = vblank (compteur `dev+16532`), appelé par le gestionnaire `921299F0` |
| `92138C50`, `92138FF0`, `92139D88` | émetteurs PM4 de groupes de registres depuis le cache du périphérique (`dev+10368/10444/10528/10548`, `RB_BLENDCONTROL0 = +10552`, `RB_COLOR_MASK = +10460`) |

Cause racine des écarts, à garder :
1. **Adresse des textures de la région E** (`>= 0xE0000000`, pages de 4 Ko) : le SDK décale la mémoire de **+0x1000** ; lire `physical_membase + (base & 0x1FFFFFFF) + 0x1000` (`PhysAddr()` dans `guest_tex.h`, et aux fetch constants de l'ombre dans `front6770.cpp` / `front_profile.cpp`). Sans cela on lit la page voisine (dégradés 128x1 d'un autre dessin, couches dupliquées).
2. Les lignes verticales, arcs et calques fantômes de l'ancien backend venaient de la lecture des fetch constants dans l'**objet D3DTexture**, sans l'état d'échantillonnage (clamp, filtres) que `SetTexture`/`SetSamplerState` fusionnent dans l'**ombre** ; le noyau n'est pas en cause.
3. Le microcode du VS du conteneur a des `vfetch` au format 0 (D3D les complète) : la façade copie le VS et recrit les `vfetch` d'après la déclaration (fente 0, format, décalage, pas ; mini-fetch et `exp_adjust` retirés), une copie par (VS, déclaration, pas) ; éléments absents : swizzle « constantes seulement ».
4. Les registres d'état sont valides **avant** le `Swap` d'origine : résoudre d'abord, puis laisser le `Swap` s'exécuter.
5. **Rampe d'affichage** : le D3D règle l'écran en BT.709 (`VdGetCurrentDisplayGamma`) et écrit une rampe PWL dans `DC_LUT_*` (tampon de commandes, non lu). Approximation par puissance **1,16** (`DASH_NGPU_GAMMA`, 1 = aucune) ; mesure 1,10 -> 4,7 ; 1,16 -> 2,4 ; 1,24 -> 4,8 (/255).
6. Alpha test : les PS XenosRecomp portent la spécialisation `1<<1` (`clip(a - g_AlphaThreshold)`, `SharedConstants+272`), non branchée (RB_COLORCONTROL `0x87000007`, test désactivé).

Registres d'état lus dans le cache (6770) : `0x2100+i` à `dev+10444+4i` (masque `0x2104`, alpha ref `0x210E`, viewport `0x210F-0x2114`), `0x2200+i` à `dev+10548+4i` (profondeur `0x2200`, blend `0x2201`, couleur `0x2202`, cull `0x2205`, VTE `0x2206`, mode `0x2208`).

## 4. HUD de XAM : second noyau, D3D 2099

Le HUD (boîtes de message, clavier, connexion) dessine avec la **copie de D3D9 2.0.2099.16 liée dans `xam.xex`** ; ses fonctions recompilées sont crochetées (`dashboards/2.0.13604.0-xam/src_xam_d3d/`) et rendues par
`native_gpu/src/hud_front.cpp` sur une **seconde instance de `ngpu_core`** (second périphérique Vulkan, EDRAM et textures propres) sans modifier le noyau. Option `NGPU_HUD` (ON pour la variante `-xam`). Détail du palier : [xam_recompile.md](xam_recompile.md).

- **Disposition** (`xenos::DeviceLayout`, `native_gpu/profiles/hud_xam13604.h`) : fetch constants et constantes canoniques ; groupes de registres canoniques + 1088 ; objets shader courants à `+12944` (pixel) et `+12948` (vertex).
- **Objets shader** : conteneur de l'ancien format (`0x102A0E0x`) copié dans l'objet (`+592` vertex, `+52` pixel) ; microcode à `+0x28` / `+0x0C`. Clé = XXH3-64 du microcode avec les `vfetch` des VS mis à zéro (D3D les remplit). Conteneurs convertis au format XenosRecomp par `tools/hud_shaders.py` (`<clé>.spv`, `<clé>.cont`).
- **Sortie** : image résolue (852 x 480, 8888, alpha) relue par `Renderer::ReadResolved` (`WaitIdle` et nouveau tampon à chaque appel : correct à ~30 images/s) et superposée à l'image du Dashboard (`ngpu::HudComposite`, appelée par `front_profile.cpp`, alpha prémultiplié, bilinéaire).
- La progression de construction passe par les boîtes de message du HUD (`common/src/dash_xam_hud_ui.cpp`) ; il n'y a plus de superposition ImGui en mode détaché.

## 5. Shaders

- **Format du conteneur** (grand-boutiste) : `flags` (`0x102A1100` pixel, `0x102A1101` vertex), `virtualSize`, `physicalSize`, `constantTableOffset` (CTAB D3DX), `definitionTableOffset` (constantes par défaut), `shaderOffset`.
  Microcode à `container + virtualSize + physicalOffset`. Pas de boucle ni d'appel dans le flot de contrôle. Clé d'exécution = XXH3-64 du microcode. Les shaders (microcode Microsoft) et leurs dérivés ne sont **jamais versionnés**.
- **Outils** : `tools/build_xenosrecomp.sh` (amont + série `patches/xenosrecomp/` -> `work/XenosRecomp-unified`, avec dxc ; forcer `CC=clang CXX=clang++`, compiler sans `NDEBUG` car les `assert` sont la seule détection d'entrée inattendue) ;
  `tools/xenos_batch.py` (un processus par shader, puis `dxc-linux -spirv`) ; `tools/dash_shaders.py` (`extract`, `translate`/`keys`, `missing`) ; `tools/hud_shaders.py` ; `tools/analyse_d3d.py` (`shaders`, `familles`, `cf`, `api`, `who`, `extract`).
- **Correctifs `patches/xenosrecomp/`** : 0001 clé de microcode (sans zstd) ; 0002 shaders sans table de constantes (`constantTableOffset == 0`) et registres `cN`/`sN` non nommés déclarés par macros par défaut ;
  0003-0004 extensions Trials (memexport, lod cube) derrière `XENOS_RECOMP_TRIALS` ; 0005 `tfetch` à indice constant masqué à 16 échantillonneurs ; 0006 `samplelevel` en VS et dédoublonnage des entrées ; 0007 emplacements `texcoord8` ; 0008 en-têtes VS 13604 ; 0009 `vfetch` indexé.
- **Dashboard 13604** : 258 conteneurs `0x102A11xx` dans `dash.xex` ; traduits à l'import. Non traduits : les VS qui lisent une texture (`tfetch` du VS, échantillonneur 19 : XenosRecomp ne déclare que 0-15 ; le noyau ne lie que 16 fetch constants), dont `2F1495350021EBE2`, créé à l'exécution
  (donc absent de `dash.xex`) ; les plantages de XenosRecomp et échecs dxc sont listés dans `<root>/shaders/<partie>-echecs.txt`. Aucun dessin sauté sur les écrans mesurés.
- **6770** : 221 conteneurs (82 VS / 139 PS) ; famille UI 70 (18 VS, 52 PS), tous traduits en SPIR-V ; **famille décodeur WMV 151 (64 VS à memexport : `outTiles/outBuffer/outImage`, non traduisibles ; 87 PS compilent mais sont inutiles sans les VS)**. SPIR-V lus dans `DASH_NGPU_SHADERS` sous `ui_<adresse>_VS|PS.spv` (ancien backend) ou `<clé>.spv`.
- **Variables** : `DASH_NGPU_SHADERS` (dossier des `<clé>.spv`) et `DASH_HUD_SHADERS` (`<clé>.spv` / `<clé>.cont` du HUD) restent prioritaires ; sinon `<game_data_root>/shaders/{dash,hud}` (fixé par `common/src/dash_app_base.cpp`).
  `DASH_NGPU_MISSING=<dossier>` : la façade y écrit les conteneurs sans SPIR-V, `dash_shaders.py missing` les traduit. Dossier absent : **une** ligne `[ngpu] ERREUR shaders natifs du Dashboard introuvables dans …` ; dossier incomplet : une ligne par shader absent.
- **Import de l'AppImage** (`tools/appimage/xenondash_import.py --native-shaders`, lancé par `AppRun` pour un build natif) : `<root>/shaders/dash` vient de `dash.xex` ; `<root>/shaders/hud` de `$flash_xam.xex` de la mise à jour (copié dans `<root>/xam.xex`, rechiffré par `xex_rekey.py` si besoin ; 34 conteneurs `0x102A0E0x`).
  Un échec d'une partie est signalé (`ÉCHEC …`) sans interrompre l'import. Outils embarqués : XenosRecomp et `dxc-linux` (`XD_XENOSRECOMP`), `xxhsum` (`XD_XXHSUM`).
  Idempotence : `<partie>.stamp` = SHA-256 du XEX source, du binaire XenosRecomp et version du format (`SHADER_VERSION`), contrôlé à chaque lancement ; `--regen-shaders` force la génération. Le binaire XenosRecomp est strippé : une AppImage reconstruite régénère une fois.
- **AppImage 13604 native** : `tools/make_appimage.sh 2.0.13604.0-xam` prend le build natif (`out/build/native`), sans `librexgpu-xenos.so` ; `usr/share/xenondash/gpu` = `native`, `AppRun` ne passe pas `--gpu_plugin=xenos`.
  Le déchiffrement du `xam.xex` 13604 demande `python3-cryptography`. Ordre de grandeur mesuré : import + génération ~20 s, 175 `.spv` Dashboard distincts, 33/34 shaders HUD (échec : `817F9550_VS`, plantage XenosRecomp).

## 6. Parité mesurée (Xvfb + lavapipe, `tools/framecmp.py`, oracle `--gpu_plugin=xenos`)

L'oracle double chaque appui de touche : comparer des états appariés par leur contenu, pas par le numéro d'appui. Plancher de bruit oracle contre oracle : 0,01/255 (13604), 1,84/255 (6770, fond animé).

| Écran (13604) | écart moyen | PSNR | pixels > 32 |
|---|---|---|---|
| Welcome (hub) | 2,44/255 | 33,5 dB | 0,49 % |
| My Xbox | 1,76/255 | 36,1 dB | 0,23 % |
| Game Library | 2,25/255 | 35,8 dB | 0,16 % |
| Xbox LIVE (Video Marketplace) | 2,55/255 | — | — |

6770, écran Games : 3,40/255, 32,8 dB, 0,14 % (ancien backend : 14,3/255, 22,9 dB, 3,4 %). Objectif (< 6/255, aucun artefact majeur) atteint. Cadence : ~25 images/s en natif sous lavapipe (~54 dessins/image pour le 13604) contre ~12 avec l'oracle.

Écarts connus (~2/255, plus fort sur le dégradé du fond, zone à 3-4/255) : rampe d'affichage approchée (§3), pas de MSAA (l'oracle fait du 2x ; la surface est déclarée `msaa=1`), filtrage des dégradés.

## 7. Limites et reste à faire

- **Présentation** : le noyau (plume) et `vk_backend.cpp` (SDL) sont deux périphériques Vulkan ; l'image passe par la mémoire (`VkPresentPacked`, A2B10G10R10 -> RGBA8 sur le CPU). À remplacer par une chaîne d'échange plume sur la fenêtre SDL (tester Wayland). Même remarque pour `ReadResolved` du HUD.
- **volk** définit des pointeurs globaux nommés comme les fonctions Vulkan et masque celles du chargeur (crash dans `vkEnumerateInstanceExtensionProperties`) : `ngpu_core/CMakeLists.txt` génère un en-tête qui les renomme (`ngpu_vk*`), force-inclus pour `plume` et `ngpu_core`. Lien : `libX11`, en-têtes Vulkan 1.4 du système.
- Reste : vertex texture fetch (échantillonneur 19), plans de coupe obliques, vidéo (WMV, **memexport** : remplacer le décodeur par un décodage logiciel), MSAA, formats de texture 565/1555/16 et formats de sommet rares (10_11_11, 2_10_10_10),
  chemins `SetStreamSource` + `SetIndices` côté 6770, cibles hors écran/`Resolve` réels (le noyau les gère, aucun dessin du Dashboard au repos ne les emploie), 17559, `LoadPipelineCache` non branché côté Dashboard, navigation hors hub non comparée au pixel près.
- Skate (`games/45410813`, détail dans [jeux.md](jeux.md)) : façade `games/45410813/native/front.cpp` sur `Core` avec `StaticShaderProvider` (348 shaders, identique octet pour octet à Skate1Native), GPU nul propre (`native/null_gpu.cpp`), profil `d3d_profile.toml` ;
  présentation par `vk_backend.cpp` sans plugin GPU. Reste : unifier les tables d'adresses des façades (6770 / 13604 / Skate), dérivation des adresses par motif à automatiser, gamma de Skate non mesuré, rejeu `.sk1c` non porté.

## 8. Build, test, diagnostic

```
tools/build_xenosrecomp.sh
python3 -I tools/dash_shaders.py extract <dash13604.pe> work/bins13604 && python3 -I tools/dash_shaders.py translate work/bins13604 work/shaders13604_keys
cmake -S dashboards/2.0.13604.0-xam -B dashboards/2.0.13604.0-xam/out/build/native -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DDASH_NATIVE_GPU=ON && cmake --build dashboards/2.0.13604.0-xam/out/build/native -j6
DASH_NGPU_SHADERS=work/shaders13604_keys dashboards/2.0.13604.0-xam/out/build/native/dash --game_data_root=<assets>   # sans la variable : <game_data_root>/shaders/dash
python3 -I tools/d3d_match.py analyze dashboards/2.0.13604.0-xam/assets/dash.xex | head                               # supported: true
cmake -S ngpu_core -B /tmp/ngpu_core_build -G Ninja && ninja -C /tmp/ngpu_core_build                                   # noyau seul (CXX=clang++ ; -DNGPU_BUILD_REPLAY=ON pour le rejeu)
python3 tools/framecmp.py --noise <oracle1.png> <oracle2.png> <natif.png>                                              # écart moyen, PSNR, grille de zones, planche contact
dashboards/2.0.13604.0-xam/e2e_natif.sh [secondes] [affichage]   # AppImage native de bout en bout : import, génération des shaders, lancement sous Xvfb + lavapipe, captures
native_gpu/tools/run_headless_native.sh <affichage> <secondes> <nom>   # DASH_BIN=<binaire oracle> pour l'oracle ; capture runs/<nom>.png
```

- Un build natif dont la version de SDK diffère de celle du `librexgpu-xenos.so` copié plante au démarrage de l'oracle : utiliser le build `-DDASH_NATIVE_GPU=OFF` pour l'oracle. 6770 : `-DDASH_NATIVE_GPU=ON` + `NGPU_CORE` (ON par défaut ; OFF = ancien backend).
- `e2e_natif.sh` a été déplacé depuis la variante supprimée : les mesures de la section 5 ne sont pas encore refaites sur `-xam`.
- **Diagnostics** (variables d'environnement) : `DASH_NGPU_DRAWINFO=<image>` (état, constantes, premiers sommets), `DASH_NGPU_DRAWRANGE=a:b` (bissection) et `DRAWSKIP=a:b`, `DASH_NGPU_REGVARY=<image>`, `REGDUMP=<image>`, `DASH_NGPU_DUMP=<image>:f.ppm` (image présentée),
  `DUMPRES` (texture résolue), `DASH_NGPU_STATS=1`, `DASH_NGPU_EVLOG`, `DASH_NGPU_GAMMA`, `DASH_NGPU_PIXLOG=<image>:x,y`, `DASH_NGPU_TEXPIX`, `DASH_NGPU_POINT=1`, `DASH_NGPU_NODRAW=1`, `DASH_NGPU_UPMAX=N`,
  `DASH_NGPU_OBSERVE=N` (+ `--gpu_plugin=xenos` : l'oracle ne dessine que les N premiers dessins UP/indexés de chaque image ; comparer avec `UPMAX` natif localise le premier dessin divergent, c'est ainsi qu'a été trouvé le +0x1000).

Historique détaillé avant condensation : `git show d96c252:docs/ngpu_core.md` et `git show d96c252:docs/rendu_natif.md`.
