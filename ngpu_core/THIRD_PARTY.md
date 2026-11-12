# ngpu_core : origine du code et licences

Le noyau est sous **GPL-3.0** (voir `../LICENSE`), comme Skate1Native dont il est tiré.

| Composant | Origine | Licence |
|---|---|---|
| `src/renderer.{h,cpp}`, `src/xenos_decode.h`, `src/shaders/`, `profiles/skate1/` (dont `front/`, `profile.h` issu de `renderer.cpp`), `tools/replay/`, `tools/d3d/` | **Skate1Native** (Kurtisone/Skate1Recompiled, dépôt GPL-3.0, commit `81805f51`), `src/native_gpu/`, `tools/native_replay/`, `tools/d3d/`. Les en-têtes d'origine sont conservés ; les écarts sont listés dans `docs/ngpu_core.md`. Copie brute au commit `cd52a1b`, séparation au suivant. | GPL-3.0 |
| Structure du moteur (tas bindless, constantes partagées XenosRecomp, modèle de présentation) | **Unleashed Recompiled** (hedge-dev), `UnleashedRecomp/gpu/video.cpp`, dont Skate1Native est dérivé | GPL-3.0 |
| `src/{shader_provider,profile,device}.*` | XenonDash (nouveau) | GPL-3.0 |
| `third_party/plume/` (RHI Vulkan seul : `plume_vulkan.*`, `plume_render_interface*.h`) | renbag/plume, avec la modification locale de Skate1Native (formats d'entrée de sommets « scaled ») | MIT (`third_party/plume/LICENSE`) |
| `third_party/plume/contrib/volk` | zeux/volk | MIT (`LICENSE.md`) |
| `third_party/plume/contrib/VulkanMemoryAllocator` | GPUOpen VulkanMemoryAllocator | MIT (`LICENSE.txt`) |
| `third_party/smol-v/` | aras-p/smol-v | domaine public / MIT (`license.txt`) |
| xxHash (en-tête, pris dans `third_party/rexglue-sdk/thirdparty/xxHash`) | Yann Collet | BSD-2 |

Aucun fichier de jeu, shader extrait ou traduit, ni capture n'est versionné ici : les shaders SPIR-V sont produits sur la machine de
l'utilisateur par XenosRecomp à partir de ses propres fichiers.
