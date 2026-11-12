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
  (`SetStreamSource`+`SetIndices`, `DrawVertices`) ; cibles hors écran/`Resolve` réels (le noyau les gère, aucun dessin du Dashboard au repos ne les emploie) ; 13604/17559/XAM.
- Non thread-safe côté noyau : la façade sérialise par un mutex.
- Le cache de pipelines (`LoadPipelineCache`) n'est pas branché côté Dashboard.

## 6. Pour Skate en recette (étape suivante, fusion_ports.md §6 étape 7)

1. Façade Skate : `profiles/skate1/front/` (live_renderer, registre de shaders, capture, hooks 5632 : 7 appels D3D + Swap) à porter sur `Core`/`ShaderProvider` : remplace
   `g_shaderCacheEntries` par `StaticShaderProvider` (cache produit par `build_shader_cache.sh`, à intégrer à la recette, clés = `XENOS_RECOMP_MICROCODE_KEY` de l'XenosRecomp unifié,
   `tools/build_xenosrecomp.sh`, à valider sur les shaders de Skate : non disponibles ici) et `Skate1Profile()`.
2. La présentation de Skate passe par le patch SDK de présentation native (`rex_gpu_set_native_guest_output_renderer`, branche `unified`) : l'image `presented()` y est déjà au bon format ;
   la façade du Dashboard utilise `vk_backend` à la place (mode détaché) ; deux présentateurs à unifier dans le noyau.
3. Profil de recette (`games/<TitleID>/d3d_profile.toml`) : adresses D3D (Begin/EndVertices, DrawIndexedVertices, DrawVertices, ClearF, BeginTiling, Resolve, Swap), adresse du périphérique
   (`0x40002200`), constantes de projection de l'image élargie, listes `interface_vertex_shaders`/`corner_vertex_shaders` (clés de microcode), plage de pitch. Le même format décrira le Dashboard
   (adresses `9212…/925C…` ci-dessus) : une seule façade à ombre, deux tables d'adresses, et la façade « par fonctions » n'est plus qu'un adaptateur pour les dessins `UP`.
4. Hooks propres au jeu (pause, quit, caméra large, intro) : hors du noyau, dans la recette (`src/`).
5. Rejeu : brancher `tools/replay` sur le cache de Skate et comparer à une capture `.sk1c` (non faisable ici).
6. Dérivation des adresses par motif : les deux XDK vus (5526 Dashboard, 5632 Skate) partagent l'ombre ; les motifs porteront sur les points d'entrée seulement.

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
