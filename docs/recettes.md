# Recettes de jeux et recompilation à la demande (P8, étapes « moteur de recettes » et « Skate en recette »)

Date : 2026-11-12. XenonDash voit le XEX d'un jeu, le recompile en natif au premier lancement (cache), puis le lance depuis le
Dashboard avec le rendu natif `ngpu_core` (aucune émulation du GPU). Premier jeu : Skate (TitleID `45410813`).
Cadre : [fusion_ports.md](fusion_ports.md) §5 (format et flux proposés), [ngpu_core.md](ngpu_core.md) (noyau de rendu).

Aucun fichier du jeu, de Microsoft, aucun shader extrait ou traduit, code généré, capture ou binaire n'est versionné : le dépôt ne
contient que des **adresses, des paramètres et du code de port**. Tout le reste vit dans le cache de l'utilisateur.

## 1. Format de recette `games/<TitleID>/`

```
games/45410813/
  recipe.toml        identité, verrou sur le XEX (SHA-256 de l'image décodée), options de build, test d'amorçage
  functions.toml     codegen : bornes de fonctions, setjmp/longjmp (adresses seulement ; Skate1Native config/skate1_functions.toml)
  runtime.toml       réglages d'exécution du jeu (touches, plein écran...) ; le moteur y ajoute game_data_root et le profil [runtime]
  d3d_profile.toml   profil de rendu natif : périphérique, adresses D3D, listes de shaders d'interface, GPU nul, constantes
  src/               hooks propres au jeu (pause, quit, intro, cadence, tuiles...), repris de Skate1Native, compilés avec le jeu
  native/            façade de rendu natif sur ngpu_core (front.cpp), GPU nul (null_gpu.cpp), xd_front.h
  tools/             outils propres au jeu : récolte des shaders dans les archives .big (RefPack), shaders de l'exécutable
  patches/           correctifs SDK/XenosRecomp propres au jeu (aucun pour Skate : tout est dans la branche `unified`)
```

`recipe.toml` (schéma 1) :

| Clé | Rôle |
|---|---|
| `schema`, `title_id`, `name` | identité |
| `[[xex]]` `version`, `media_id`, `sha256_image` | **verrou** : seul un XEX dont l'image décodée a ce hachage est recompilé |
| `[codegen]` `project`, `out_directory`, `files` | nom du projet ReXGlue, dossier de sortie, fichiers d'analyse |
| `[shaders]` | scripts de récolte (`harvest`, `image_shaders`), dossier d'archives |
| `[render]` `mode` | `native` (ngpu_core) ; l'émulation n'est pas proposée (zéro émulation) |
| `[build]` | `min_ram_gb`, `min_disk_gb`, `arch_flags`, `estimate_minutes` (affichée par l'écran de progression) |
| `[bootstrap]` | durée du test d'amorçage et motifs attendus dans le journal |

Le **profil D3D** (`d3d_profile.toml`) est transformé par le moteur en `d3d_profile.generated.h` (constantes) et
`d3d_hooks.generated.inc` (un crochet `REX_HOOK_RAW` par `[[hook]]`, qui appelle la fonction d'origine puis la façade). Sections :
`[device]` (adresse du `D3DDevice`, taille de l'ombre), `[functions]` (points d'entrée de la XDK D3D liée dans le XEX), `[constants]`
(constantes 16/9 de l'image élargie), `[null_gpu]` (voir §4), `[profile]` (clés de microcode des vertex shaders d'interface, plage de pitch de la scène), `[runtime]`.

## 2. Moteur : `tools/recipe_build.py`

```
tools/recipe_build.py identify <dossier|default.xex|iso>   TitleID, version, SHA-256 de l'image décodée, recette trouvée ?
tools/recipe_build.py build    <dossier|default.xex|iso>   construit (ou réutilise) le cache ; --force, --stages a,b, --jobs N
tools/recipe_build.py status | launch-info <source>        état et commande de lancement (JSON) : le Dashboard s'en sert
tools/recipe_build.py clean <TitleID>                      supprime le cache du jeu
```

Étapes (affichées une à une, avec une durée estimée tirée de la recette) :
`identify` (en-tête XEX, `rexglue dump-xex`, SHA-256 de l'image, recette, verrou) → `prepare` (disque, RAM, outils) → `extract` (la source reste où
elle est, référencée par chemin ; une ISO est extraite dans `…/sources/`) → `scaffold` (projet dans le cache : sources de la recette, manifeste,
CMakeLists du gabarit `tools/templates/game/`, profil D3D généré, `skate1.toml`) → `codegen` (`rexglue codegen`) → `shaders`
(récolte dans les archives du jeu + shaders de l'exécutable, XenosRecomp unifié, `shader_cache.cpp`) → `compile` (cmake + ninja, clang hôte) →
`bootstrap` (le binaire démarre sous Xvfb et doit journaliser le noyau et sa première image native).

**Cache** : `$XDG_DATA_HOME/xenondash/games/<TitleID>/<clé>/` (défaut `~/.local/share`), clé = SHA-256 de (image du XEX, schéma, SDK installé, XenosRecomp,
section `[codegen]` + fichiers de codegen). Contenu : `project/` (sources copiées, code généré, build), `shaders/`, `bin/` (binaire et bibliothèques),
`build.ok` (révision de la recette, durées), `progress.json`, journaux (`codegen.log`, `shaders.log`, `compile.log`, `bootstrap.log`, `build.log`), `stamps/`.
Changer les hooks ou la façade ne refait ni le codegen ni les shaders : ninja ne recompile que les fichiers touchés.

**Progression** : `progress.json` (étape, pourcentage, message, durée écoulée, estimation) et lignes `[recipe] …`. Le Dashboard l'affiche (§5).

**Compilation** : clang hôte (testé : clang 23) avec `-march=x86-64-v2`, tâches parallèles = min(cœurs, RAM disponible / 2,2 Go). Un conteneur
(podman/docker, Ubuntu 22.04, clang 20) reste possible pour un binaire portable (SteamOS) : `Containerfile` de Skate1Native (`tools/steamdeck/`), non imposé ici
et non intégré (l'option `--container` le signale).

## 3. Rendu de Skate sur ngpu_core

`native/front.cpp` est la façade de Skate1Native (`live_renderer.cpp` + `shader_registry.cpp` + `native_gpu.cpp`, commit `81805f51`) portée sur `native_gpu::Core` :
registre de shaders par clé de microcode (XXH3), `StaticShaderProvider` (le cache XenosRecomp compilé avec le jeu, 348 shaders) copié dans le `MapShaderProvider` du
noyau, enregistrement des appels D3D (dessins, effacements, tuiles, resolves) rendus au `Swap` par un fil à part, image élargie (`native_gpu_aspect`, `native_gpu_render_scale`),
profil `Profile` tiré de `d3d_profile.toml`. **Aucun plugin GPU** (`config.gpu_plugin` vidé) : le noyau est la seule voie de rendu ; la présentation passe par le
présentateur Vulkan direct du Dashboard (`native_gpu/src/vk_backend.cpp`, fenêtre SDL du SDK).

## 4. GPU nul de Skate

Sans émulation, personne ne lit le tampon de commandes ni n'écrit les compteurs que la XDK scrute. Le Dashboard 6770 y répond par un « GPU nul »
(`native_gpu/src/null_gpu.cpp`) : un fil publie « tout est consommé » dans le bloc de retour (`*(dev+10896)` = valeur `dev+10908`) et un fil de
vblank appelle le gestionnaire d'interruption graphique à 60 Hz. Les D3D de Skate (XDK 5632) et du Dashboard (5526) ont le **même code** pour ces fonctions, retrouvé par
comparaison de motifs (désassemblage du Dashboard contre l'image décodée de Skate) :

| Rôle | Dashboard 6770 | Skate |
|---|---|---|
| attente de fence | `9212A418` | `82CB11C0` |
| attente de drainage de l'anneau (+60) | `92129788` | `82CB0060` |
| fence secondaire (+4) | `921296F0` | `82CAFF88` |
| gestionnaire de vblank | `92121210` | `82CCB758` |

Décalages `+10896` / `+10908` identiques ; plus loin dans le périphérique ils diffèrent (ex. 14884 contre 14876), d'où l'intérêt de la table par jeu.

## 5. Intégration au Dashboard

`games.toml` : `[game.45410813] name = "Skate"`, `recipe = "<dossier du jeu | default.xex | ISO>"` (à la place de `native`). `LaunchGame` (disque simulé, My Games, F5) :
1. interroge `recipe_build.py launch-info` : si le cache est construit, la commande « native » est `./skate1` dans `<cache>/bin` et le lancement suit le chemin P5 (le Dashboard cède la place, relancé au retour) ;
2. sinon il lance la construction dans un **processus détaché** (journal `<cache>/build.log`), **reste vivant**, affiche une **fenêtre de progression ImGui** (étape, barre, durée écoulée/estimée, journal),
   journalise toutes les 5 s, envoie `notify-send` s'il existe, et **lance le jeu tout seul** à la fin (échec : message et journal, le Dashboard continue).

L'écran de progression est une fenêtre ImGui du Dashboard (versions rendues par le plugin `xenos`, ou à terme par `ngpu_core` quand ImGui y sera porté, voir ngpu_core.md §5) ; il ne s'agit pas d'une scène XUI d'origine.

## 6. Mesures et reste

(voir §7 après la première construction complète)
