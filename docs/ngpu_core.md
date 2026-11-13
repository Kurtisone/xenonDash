# ngpu_core : le noyau de rendu natif commun (P8, étape 3)

Date : 2026-11-12. Bibliothèque de rendu natif Vulkan commune au Dashboard et aux jeux, tirée de la couche D3D native de
Skate1Native (`../Skate1Recompiled`, GPL-3.0, commit `81805f51`, lecture seule) et branchée sur le Dashboard 6770 par une
**façade D3D**. Architecture : [fusion_ports.md](fusion_ports.md) §3.6 ; prototype d'origine : [rendu_natif.md](rendu_natif.md) §11-12.

## 1. Résultat

- Palier 1 : `ngpu_core/` compile seul (clang, Vulkan système), sans dépendance au jeu ni au SDK.
- Palier 2 : `-DDASH_NATIVE_GPU=ON` (6770) rend l'écran Games **par le noyau** (option `NGPU_CORE`, défaut ON ; OFF = ancien backend `vk_backend`).
- **Parité** (écran Games, Xvfb/lavapipe, 40 s, `tools/framecmp.py` contre l'oracle `--gpu_plugin=xenos`, build par défaut) :

| Rendu | écart moyen | PSNR | pixels à écart > 32 |
|---|---|---|---|
| plancher de bruit (oracle contre oracle) | 1,84/255 | 36,0 dB | 0,00 % |
| ancien backend (`NGPU_CORE=OFF`) | 14,3/255 | 22,9 dB | 3,4 % |
| **noyau (façade 6770)** | **3,40/255** (4,15 contre une 2e exécution) | **32,8 dB** | **0,14 %** |

  Référence du jalon P7 : 14,4/255, 22,7 dB. Visuellement : plus de lignes verticales (x≈235/1040), d'arcs ni de calques fantômes ;
  l'image est celle de l'oracle (captures locales `dashboards/2.0.6770.0/runs/preuves/core_*.png`, non versionnées).
  L'écart restant (≈ 1,6/255 au-dessus du bruit) : MSAA 2x de l'oracle (la surface est déclarée `msaa=1`, le noyau rend sans), rampe
  d'affichage approximée (§5), filtrage des gradients.
- Cadence sous lavapipe : ≈ 26 images/s (180 dessins/image ; une copie GPU→CPU→GPU de l'image par la mémoire, cf. §5).
- Build par défaut (OFF) inchangé : smoke 6770 et 13604 OK (§7).

## 2. Structure

```
ngpu_core/
  CMakeLists.txt        cible `ngpu_core` (STATIC) + `plume` ; NGPU_BUILD_REPLAY ; renomme les symboles volk (ngpu_vk*), voir §5
  THIRD_PARTY.md        origines et licences (GPL-3.0 ; plume/volk/VMA MIT, smol-v, xxHash)
  src/
    renderer.{h,cpp}    le moteur (de Skate1Native) : cibles EDRAM, resolves, textures (détuilage), tampons, pipelines, descripteurs bindless
    xenos_decode.h      décodage Xenos : ombre canonique du périphérique, fetch constants, formats, détuilage 2D/3D/cube/mips (inchangé)
    shader_provider.h   NOUVEAU : ShaderProvider (SPIR-V par clé de microcode) + MapShaderProvider (blobs ou fichiers)
    profile.h           NOUVEAU : Profile (ce que le noyau doit savoir d'un jeu)
    device.{h,cpp}      NOUVEAU : Core = périphérique plume + Renderer + MapShaderProvider
    shaders/            resolve.hlsl + native_shaders.h (SPIR-V du resolve et de l'empaquetage, générés par tools/compile_native_shaders.sh)
  profiles/skate1/      profil de Skate : profile.h (ex-constantes du renderer), static_shader_provider.h (cache compilé smol-v/zstd),
                        shader_cache.h, front/ = façade de Skate1Native telle quelle, EN RÉFÉRENCE (non compilée : étape « Skate en recette »)
  third_party/plume     RHI Vulkan seul (renderbag/plume), volk, VMA ; third_party/smol-v
  tools/replay          rejeu hors ligne d'une capture .sk1c (NGPU_BUILD_REPLAY=ON, demande le cache de Skate ; compile avec un cache vide)
  tools/d3d             compile_native_shaders.sh (sortie identique à l'original octet pour octet), capture.py (lecteur de captures)
native_gpu/             façade 6770 : front6770.cpp (NGPU_CORE=ON) ; hooks_draw.cpp + vk_backend.cpp = ancien backend (OFF)
```

Capture/rejeu : le format `.sk1c`, l'enregistreur (`profiles/skate1/front/frame_capture.cpp`, branché sur les adresses de Skate) et l'outil de rejeu
sont repris ; le rejeu compile mais n'a **pas pu être exécuté** ici (il faut les fichiers du jeu pour le cache de shaders de Skate). La
capture côté Dashboard n'est pas portée (le mode `DASH_NGPU_DUMP*` de la façade dumpe les images).

## 3. API du noyau

```cpp
native_gpu::Core core;                       // device.h
core.Initialize(physical, container_lookup, watcher, profile, &error);
core.shaders().AddFile(key, "x.spv");        // ou AddBlob ; clé = XXH3-64 du microcode (XENOS_RECOMP_MICROCODE_KEY)
native_gpu::Renderer& r = core.renderer();
r.BeginFrame();
r.Clear(ClearCall); r.Draw(DrawCall); r.Resolve(ResolveCall);
r.EndFrame(front_buffer_address);            // soumet ; r.presented() = image (A2B10G10R10, une image de retard) ; r.WaitIdle()
r.SetRenderScale(f); r.SetWidth(w); r.SetGammaRamp(256 entrées 10 bits) ; r.LoadPipelineCache(path) / SavePipelineCache()
```

- **Ombre canonique du périphérique** (`xenos::DeviceState{const uint8_t* data}`, octets grand-boutistes, disposition de xenos_decode.h) : fetch constants
  `+0x480` (32 × 24 o), constantes de sommets `+0x780`, de pixels `+0x1780`, booléens `+0x2780/+0x2790`, registres Xenos par groupes
  (`+10368` 0x2000, `+10444` 0x2100, `+10528` 0x2180, `+10548` 0x2200, `+10596` 0x2280, `+10680` 0x2300, `+10832` 0x2380). Chaque appel
  (`DrawCall`, `ClearCall`, `ResolveCall`) en porte un instantané : le noyau n'a aucun état « D3D ».
- `DrawCall` : clés de microcode du VS/PS, primitive Xenos (1, 2, 3, 4, 6, 13), comptes, **partie physique du VS telle que D3D l'a patchée**
  (le noyau y lit les `vfetch` : format, décalage, pas, fente), indices en mémoire physique (`indices_address`) ou en ligne (`inline_indices`, nouveau),
  sommets en ligne (`inline_vertices`, grand-boutistes, tampon unique).
- `Resolve` lit la destination dans les registres `RB_COPY_DEST_BASE/PITCH` (0x2319/0x231A) de l'ombre ; `EndFrame(adresse)` empaquette la texture résolue à cette adresse.
- `ShaderProvider` : le noyau ne lit plus de tables compilées ; il demande un SPIR-V non compressé par clé. Implémentations : `MapShaderProvider` (Dashboard : un fichier
  par shader, `work/shaders_out/ui_<adresse>_VS|PS.spv`, enregistrés à la création de l'objet D3D) et `StaticShaderProvider` (Skate : tables du cache compilé).
- `WriteWatcher` : surveillance d'écriture de la mémoire physique (pages protégées, callbacks du SDK) pour ne reconvertir que les textures et tampons modifiés ; la façade
  reprend l'implémentation de Skate (`PhysicalWriteWatcher`).
- `Profile` : `interface_vertex_shaders`, `corner_vertex_shaders` (minicarte), `scene_pitch_min/max` (surfaces concernées par l'échelle de rendu et l'image élargie).
  Dans Skate1Native c'étaient des constantes du renderer ; le Dashboard n'en a pas (plage vide).

## 4. Façade 6770 (`native_gpu/src/front6770.cpp`)

Mêmes fonctions D3D que le prototype (cartographie : rendu_natif.md §11.2), mais chaque appel suivi alimente le noyau :

| Hook | Rôle dans la façade |
|---|---|
| `9212EE10/9212ED00` CreateVertex/PixelShader | clé de microcode, copie du conteneur, enregistrement du SPIR-V |
| `9212EB48/9212EA00` CreateVertexDeclaration / Set | éléments de sommets (usage, indice, décalage, format) |
| `9212E7E8/9212E4E8`, `92123F20` SetVertex/PixelShader, SetTexture | objets liés |
| `925CCB68`, `925CD0E0`, `925CC6B8`+`925CCB58` DrawVerticesUP, DrawIndexedVerticesUP, Begin/EndVertices | un `DrawCall` ; rect list (8), fan (5) et quads indexés développés en triangles côté façade |
| `92128AF0` Clear | `ClearCall` |
| `92121EE8` Swap | `Resolve` de la surface vers un tampon fictif (`0x1F000000`) + `EndFrame` + présentation |

Trouvailles qui font la parité :
1. **L'ombre du périphérique 6770 est celle de Skate** (vu dans `SetTexture`/`Set*ShaderConstantF` : fetch constants `+48*24`, constantes `+120*16` et `+376*16`). La façade n'a donc rien
   à synthétiser pour cela : `BuildShadow` copie les 13 Ko du périphérique. Les artefacts de l'ancien backend (lignes verticales, arcs, calques fantômes) venaient de la lecture
   des fetch constants dans l'**objet D3DTexture**, sans l'état d'échantillonnage (clamp, filtres) que `SetTexture`/`SetSamplerState` fusionnent dans l'ombre : les dégradés 128x1 se
   répétaient aux bords. Le noyau n'est pas la cause : c'est la source de l'état. (Le noyau de Skate seul ne les « résolvait » pas ; la lecture de l'ombre, si.)
2. Texture en région E (`>= 0xE0000000`) : adresse + `0x1000` (§12.1 de rendu_natif.md), appliquée aux fetch constants de l'ombre.
3. Le microcode du VS du conteneur a des `vfetch` au format 0 (D3D les complète) : la façade en fait une copie dont les `vfetch` sont recrits d'après la déclaration (fente 0, format, décalage, pas ; mini-fetch et `exp_adjust` retirés) ;
   éléments absents de la déclaration : swizzle « constantes seulement ». Une copie par (VS, déclaration, pas).
4. Les registres d'état sont valides **avant** le `Swap` d'origine, pas après (il les remet à autre chose) : la façade résout puis laisse le `Swap` s'exécuter.
5. Rampe d'affichage : le D3D du Dashboard règle l'écran en BT.709 (`VdGetCurrentDisplayGamma`) et écrit une rampe PWL dans `DC_LUT_*` ; ces écritures partent dans le tampon de
   commandes, non lu ici. Approximation mesurée contre l'oracle : puissance 1,16 (`DASH_NGPU_GAMMA`, 1 = aucune). Ce seul réglage fait passer l'écart de 13,2 à 3,4/255.

Diagnostics (tous par variables d'environnement) : `DASH_NGPU_DRAWINFO=<image>` (état et premiers sommets de chaque dessin), `DASH_NGPU_DRAWRANGE=a:b`
(bissection : seuls les dessins a..b de chaque image), `DASH_NGPU_REGVARY=<image>` (registres qui varient), `DASH_NGPU_DUMP=<image>:f.ppm` (image présentée),
`DASH_NGPU_DUMPRES=<image>:f.ppm` (texture résolue), `DASH_NGPU_STATS=1` (statistiques du noyau), `DASH_NGPU_POINT=1`, `DASH_NGPU_NODRAW=1`, `DASH_NGPU_SHADERS=<dossier>`.

## 5. Limites et points d'attention

- **Présentation** : le noyau (plume) et `vk_backend.cpp` (présentation SDL) sont deux périphériques Vulkan ; l'image passe par la mémoire (`VkPresentPacked`, conversion A2B10G10R10 → RGBA8 sur le CPU).
  À remplacer par une chaîne d'échange plume sur la fenêtre SDL (à tester aussi sous Wayland) quand le rendu devient la voie principale.
- **volk** définit des pointeurs globaux nommés comme les fonctions Vulkan : liés dans le même exécutable que libvulkan, ils masquent les fonctions du chargeur (crash dans
  `vkEnumerateInstanceExtensionProperties` de `vk_backend`). `ngpu_core/CMakeLists.txt` génère un en-tête qui les renomme (`ngpu_vk*`), force-inclus en PRIVATE pour `plume` et `ngpu_core`.
- Dépendance de lien : `libX11` (plume lit les attributs de la fenêtre X11 pour sa chaîne d'échange, non utilisée ici) ; en-têtes Vulkan du système (plume compile avec Vulkan 1.4 headers).
- **Non fait** : superposition ImGui (lanceur F5, remplaçants XAM : le SDK dessine ImGui par le `ImmediateDrawer` du présentateur, absent en mode détaché ; voie prévue : une passe
  `ImDrawData` dans le noyau, ou un `ImmediateDrawer` sur plume, plus l'hébergement de la boucle UI) ; MSAA ; vidéo (A7) ; formats de texture 565/1555/16 ; autres chemins de dessin
  (`SetStreamSource`+`SetIndices`, `DrawVertices`) ; cibles hors écran/`Resolve` réels (le noyau les gère, aucun dessin du Dashboard au repos ne les emploie) ; 17559/XAM (le 13604 est fait : §8).
- Non thread-safe côté noyau : la façade sérialise par un mutex.
- Le cache de pipelines (`LoadPipelineCache`) n'est pas branché côté Dashboard.

## 6. Skate en recette (FAIT 2026-11-12, étape suivante de fusion_ports.md §6 étape 7)

Détail et mesures : [recettes.md](recettes.md). Ce qui était prévu ici et ce qui a été fait :

1. Façade Skate : reprise de `profiles/skate1/front/` (live_renderer, registre de shaders) dans `games/45410813/native/front.cpp`, sur `Core` : `StaticShaderProvider` (cache produit par
   XenosRecomp unifié, **identique octet pour octet** à celui de Skate1Native : 348 shaders, SHA-256 `06880cfb…`) copié dans le `MapShaderProvider` du noyau, profil tiré de `d3d_profile.toml`. FAIT.
2. Présentation : au lieu du patch SDK de présentation native, **aucun plugin GPU** ; le fil de rendu présente l'image `presented()` par le présentateur Vulkan du Dashboard (`native_gpu/src/vk_backend.cpp`,
   fenêtre SDL du SDK), avec un GPU nul propre à Skate (`native/null_gpu.cpp`, fonctions D3D retrouvées par motif depuis le Dashboard 6770). Le présentateur reste une copie CPU (un seul tampon 1280x720 : image élargie/échelle de rendu
   recadrées) : à remplacer par la chaîne d'échange plume (§5). FAIT en l'état.
3. Profil de recette `games/<TitleID>/d3d_profile.toml` : adresses D3D, `[null_gpu]`, constantes de l'image élargie, listes de shaders d'interface, plage de pitch ; le moteur en tire un en-tête et les crochets D3D. FAIT pour Skate
   (le Dashboard 6770 garde sa façade `front6770.cpp` : même noyau, deux tables d'adresses restent à unifier).
4. Hooks propres au jeu (pause, quit, caméra large, intro, cadence) : dans la recette (`src/`). FAIT.
5. Rejeu `.sk1c` : non repris (la capture de Skate1Native n'est pas portée dans la façade de recette). Reste.
6. Dérivation des adresses par motif : faite à la main pour le GPU nul (§4 de recettes.md) ; à automatiser pour un nouveau XEX (recompilation générique).
7. Reste mesuré : gamma d'affichage de Skate (puissance 1,16 reprise du Dashboard, non mesurée contre une référence), MSAA, parité pixel contre l'émulation de Skate1Native (non comparée ici).

## 7. Reproduire

```
tools/build_xenosrecomp.sh                                   # XenosRecomp unifié + dxc (work/XenosRecomp-unified)
cmake -S dashboards/2.0.6770.0 -B dashboards/2.0.6770.0/out/build/native -DDASH_NATIVE_GPU=ON   # NGPU_CORE=ON par défaut
cmake --build dashboards/2.0.6770.0/out/build/native -j10
native_gpu/tools/run_headless_native.sh 90 40 core_natif                                          # capture runs/core_natif.png
DASH_BIN=dashboards/2.0.6770.0/out/build/linux-amd64-relwithdebinfo/dash native_gpu/tools/run_headless_native.sh 91 40 core_oracle
python3 tools/framecmp.py --noise <oracle1.png> <oracle2.png> <natif.png>
cmake -S ngpu_core -B /tmp/ngpu_core_build -G Ninja && ninja -C /tmp/ngpu_core_build          # noyau seul (CXX=clang++ ; -DNGPU_BUILD_REPLAY=ON pour le rejeu)
tools/port_dashboard.sh firmware/2.0.6770.0 2.0.6770.0 --stages build,smoke                  # build par défaut (OFF) : inchangé
```
(Le build natif avec une version de SDK différente de celle du `librexgpu-xenos.so` copié dans son dossier plante au démarrage de l'oracle : utiliser le build par défaut pour l'oracle.)


## 8. Dashboard NXE 2.0.13604 (palier N1, 2026-11-13) : rendu natif sans plugin `xenos`

`-DDASH_NATIVE_GPU=ON` sur `dashboards/2.0.13604.0` : aucun plugin GPU, aucun lecteur de tampon de commandes ; le plugin `xenos` ne sert plus que d'**oracle**
(`DASH_NGPU_DISABLE=1 --gpu_plugin=xenos` ou le build par défaut). Build par défaut (option OFF) inchangé (la configuration n'inclut pas `native_gpu`).
La variante `-xam` n'est pas touchée (ses fichiers non plus : `common/` n'est pas modifié par ce palier).

### 8.1 Fonctions D3D (XDK 2.0.20158) : adresses et confiance

Retrouvées par `tools/d3d_match.py` (le matcheur donnait 9 rôles sur 11), puis **validées à la main** (désassemblage `tools/ppc_dis.py`, comparaison avec la famille 5632/6132, vérification
du graphe d'appels et des arguments) et apprises : famille `dash-13604` (`tools/d3d_signatures/dash-13604.json`, références dans `references.toml`). `d3d_match.py analyze` sur le XEX :
**supported = true**, 20 rôles à confiance 1,0 (la marge d'un rôle ne compte plus un jumeau déjà retenu par un autre rôle : `Resolve`/`ResolveInner`).
La famille est apprise sur le Dashboard lui-même : le test est circulaire pour ses adresses ; ce qui n'est pas circulaire est la lecture de code ci-dessous et le rendu (§8.4).

| Rôle | Adresse | Validation |
|---|---|---|
| BeginVertices / EndVertices | `927E0A58` / `927E0A48` | (dev, prim, nb, pas) -> pointeur ; le vidage d'état écrit les groupes de registres `+10368/10444/10528/10548/10596/10680/10832` (identiques à la XDK 5632), constantes `+1920`/`+6016` |
| DrawVerticesUP | `927E0F18` | enveloppe de BeginVertices (+ copie, EndVertices fondu : `+13652 -> +48`) |
| DrawIndexedVerticesUP | `927E14A0` | (dev, prim, min, nb, nbIdx, idx, format, sommets, pas à `r1+84`), comme le 6770 |
| DrawVertices / DrawIndexedVertices | `927E1540` / `927E1958` | flux du périphérique ; tampon d'indices à `+12612` |
| Clear (ClearF) | `92195E98` | (dev, drapeaux, rect*, couleur* de 4 float, f1 = z, **r8 = stencil**) ; appelé par `92195FC0` (Clear public) et par BeginTiling |
| BeginTiling | `92196370` | appelle ClearF avec 63 ; non suivi (rendu sans tuiles, l'effacement passe par ClearF) |
| Resolve | `921A6908` (enveloppe) / `921A57B8` (interne, 2 arguments de plus) | `RB_COPY_DEST_BASE` `+10780` ; Swap public `9218F138` = Resolve puis `9218EAB0` |
| Swap | `9218EAB0` | appelle `VdSwap` |
| CreateVertexShader / CreatePixelShader | `9219D0F0` / `9219CF08` | (conteneur) -> objet ; objet VS : en-tête 40 o + 832 o d'état de vfetch, partie virtuelle copiée à `+872`, **pointeur du microcode physique (patché par D3D) à `+32`** ; objet PS : virtuelle à `+40`, physique `+24` |
| SetVertexShader / SetPixelShader | `9219C8C8` / `9219C6C0` | courants à `dev+12872` / `dev+12868` (+184 par rapport à la 5632) |
| SetTexture | `92191348` | (dev, étage, texture, drapeaux) ; fetch constants à `dev+(étage+48)*24` = `+0x480` |
| VblankHandler | `9218E290` | appelé par `92196F30` (interruption) ; compteur `dev+16760` |
| WaitFence / WaitDrain / WaitSecond | `92197358` / `92196CA8` / `92196C10` | `+10896`, `+10908`, mot `+60`, mot `+4` (identiques au 6770 et à Skate) |

Disposition du périphérique : **canonique** pour tout ce que lit le noyau (fetch `+0x480`, constantes `+0x780`/`+0x1780`, booléens `+0x2780`, registres), sauf `vertex_shader 12872`,
`pixel_shader 12868`, `index_buffer 12612` (`device_layout.h`, `Canonicalize`). Le D3D 20158 écrit ses constantes et ses fetch constants **directement dans le périphérique** (`stfs 6016(r31)`,
`lwz/stw 1152(r31)`) : aucun crochet `Set*ShaderConstantF`/`SetTexture` n'est nécessaire ; les `SetVertexShaderConstantF`/`SetPixelShaderConstantF` du matcheur (0,2) n'existent pas sous cette forme. Plans de coupe
utilisateur : `dev+10272` (6 x float4), hors ombre (§8.3).

### 8.2 Façade (`native_gpu/`)

- `native_gpu/CMakeLists.txt` : variable `NGPU_PROFILE` (6770 par défaut ; le CMakeLists de 13604 la fixe à 13604). `profiles/dash6770.h`, `profiles/dash13604.h` : adresses (macros `NGP_*`) et décalages ;
  `null_gpu.cpp` et `d3d_trace.cpp` les lisent (comportement 6770 inchangé ; l'attache du GPU nul attend maintenant que `*(dev+fence_ptr)` soit non nul).
- `src/front_profile.cpp` : façade 13604 (même principe que `front6770.cpp` et que la façade générique de Skate, rendu **synchrone** sous un verrou). Crochets `NGPU_HOOK(NGP_*)` (enveloppeurs de `d3d_trace.cpp`,
  entrées `d3d13604_entries.inc` : 1 492 fonctions de la bibliothèque D3D, pour le traçage `DASH_D3D_TRACE`) :
  GPU nul (WaitFence/Drain/Second + vblank 60 Hz), BeginVertices (dessin ouvert, clos au prochain appel D3D suivi car `DrawVerticesUP` fond EndVertices), DrawIndexedVerticesUP, DrawVertices/DrawIndexedVertices,
  ClearF, Resolve, Swap (`EndFrame` du noyau, présentation par `vk_backend.cpp`). Ombre = `Canonicalize` du périphérique, adresses de textures de la région E corrigées (+0x1000, comme 6770).
  Le microcode du VS est celui que **D3D a patché** dans l'objet (lu après l'appel d'origine, copié au BeginVertices) : pas de table de déclarations ; les éléments du quatrième sommet des rect lists en sont déduits
  (vfetch). Les objets shader sont lus **dans leur propre mémoire** au premier dessin (clé = XXH3-64 du microcode) : aucun crochet de création indispensable.
- `src/write_watcher.h` : `PhysicalWriteWatcher` (copie de celui de `front6770.cpp`, à unifier).
- `ngpu_core` : `DrawCall::has_scissor/scissor` (rectangle de coupe en pixels invités, §8.3) ; sans effet pour 6770 et Skate (faux par défaut).
- `app.h` de 13604 : `gpu_plugin` vidé sous `DASH_NATIVE_GPU` (sauf `DASH_NGPU_DISABLE`).
- Diagnostics (variables d'environnement) : `DASH_NGPU_EVLOG`, `DASH_NGPU_DRAWINFO=<image>` (état, constantes, plans), `DASH_NGPU_DRAWRANGE=a:b`, `DASH_NGPU_DRAWSKIP=a:b`, `DASH_NGPU_REGVARY=<image>`,
  `DASH_NGPU_REGDUMP=<image>`, `DASH_NGPU_DUMP=<image>:f.ppm`, `DASH_NGPU_STATS`, `DASH_NGPU_GAMMA`, `DASH_NGPU_SHADERS` (dossier des `<clé>.spv`), `DASH_NGPU_MISSING` (conteneurs des shaders absents).

### 8.3 Trouvaille : plans de coupe utilisateur

Premier rendu natif : un texte « No profile… Try setting up… » superposé, hors de l'oracle, en bas à droite. Cause : XUI clippe ses textes avec les **plans de coupe utilisateur** (`PA_CL_CLIP_CNTL` `0x2204`, bits 0-5,
`0x0008000F`), valeurs à `dev+10272` ; ici deux plans opposés (x >= c et x <= c) : fente de largeur nulle, texte entièrement coupé. Le noyau ne sait pas couper (pas de `ClipDistance`
dans les SPIR-V de XenosRecomp) : la façade convertit les plans alignés sur les axes en rectangle de coupe (conversion NDC -> pixels par le viewport) transmis au noyau ; un rectangle vide saute le dessin ; les plans obliques sont
signalés et ignorés. Après correction : disparition de l'artefact (Welcome : 2,57 -> 2,42/255).

### 8.4 Shaders

`tools/dash_shaders.py` : `extract` (conteneurs `0x102A11xx` de l'image : 258), `translate`/`keys` (XenosRecomp unifié + dxc, `tools/xenos_batch.py`, puis renommage `<XXH3 du microcode>.spv`), `missing`
(boucle « jouer -> reconstruire » : la façade écrit le conteneur d'un shader sans SPIR-V dans `DASH_NGPU_MISSING`, l'outil le traduit). Résultat : **116 / 118** shaders d'interface traduits (97 PS, 19 VS ; 106 clés distinctes).
Non traduits : 3 vertex shaders qui lisent une texture (`tfetch` du VS, échantillonneur 19 : XenosRecomp ne déclare que les échantillonneurs 0-15 ; `DotEffectSize`), dont `2F1495350021EBE2` créé à l'exécution ; aucun dessin sauté
sur les écrans mesurés (le noyau ne lie que les 16 premières fetch constants : la lecture de texture en VS demanderait aussi un changement du noyau). Rien n'est versionné.

### 8.5 Parité mesurée (Xvfb :94 + lavapipe, `tools/framecmp.py`, même script de touches, oracle = `--gpu_plugin=xenos`)

L'oracle double chaque appui de touche (images plus lentes : ≈ 12 images/s contre ≈ 25 en natif) : les états comparés sont appariés par leur contenu, pas par le numéro d'appui.
Plancher de bruit (oracle contre oracle, deux exécutions) : **0,01/255**.

| Écran | écart moyen | PSNR | pixels à écart > 32 |
|---|---|---|---|
| Welcome (hub) | 2,44/255 | 33,5 dB | 0,49 % |
| My Xbox | 1,76/255 | 36,1 dB | 0,23 % |
| My Xbox, défilement vers Game Library | 1,81/255 | 35,8 dB | 0,27 % |
| Xbox LIVE (lame Video Marketplace) | 2,55/255 | — | — |
| Game Library | 2,25/255 | 35,8 dB | 0,16 % |

Objectif (< 6/255, aucun artefact majeur) atteint sur les quatre écrans ; visuellement identiques (regardés un par un). Écart restant (≈ 2/255, plus fort sur le dégradé du fond : zone 3 à 4/255) : rampe d'affichage
approchée par la puissance 1,16 (réglage mesuré : 1,10 -> 4,7 ; 1,16 -> 2,4 ; 1,24 -> 4,8), pas de MSAA, filtrage des dégradés. Cadence : ≈ 25 images/s sous lavapipe (≈ 54 dessins/image).
Reste : écrans à vertex texture fetch (§8.4), plans de coupe obliques, vidéo (WMV, `memexport`), superposition ImGui, présentation par copie CPU (§5), 17559, variante `-xam`.
Navigation hors hub (Xbox LIVE : boîte de dialogue « temporarily unavailable ») non comparée au pixel près.

### 8.6 Reproduire

```
tools/build_xenosrecomp.sh
python3 -I tools/dash_shaders.py extract <dash13604.pe> work/bins13604 && python3 -I tools/dash_shaders.py translate work/bins13604 work/shaders13604_keys
cmake -S dashboards/2.0.13604.0 -B dashboards/2.0.13604.0/out/build/native -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DDASH_NATIVE_GPU=ON && cmake --build dashboards/2.0.13604.0/out/build/native -j6
DASH_NGPU_SHADERS=work/shaders13604_keys dashboards/2.0.13604.0/out/build/native/dash --game_data_root=<assets>      # natif
python3 -I tools/d3d_match.py analyze dashboards/2.0.13604.0/assets/dash.xex | head                                      # supported: true
```
