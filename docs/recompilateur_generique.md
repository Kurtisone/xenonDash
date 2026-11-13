# Recompilateur générique (P8, étape 6)

Date : 2026-11-12. XenonDash recompile en natif **tout XEX sans recette** (ou dont le hachage n'est pas verrouillé) et le lance depuis le
Dashboard, avec le rendu natif `ngpu_core`. Contrainte dure : **zéro émulation** (ni plugin GPU `xenos`, ni Xenia). Un jeu que le mode
générique ne sait pas rendre nativement est **refusé avec un diagnostic précis**, jamais basculé sur l'émulation.
Cadre : [recettes.md](recettes.md) (mode recette), [ngpu_core.md](ngpu_core.md), [fusion_ports.md](fusion_ports.md).

Aucun fichier du jeu, aucun shader, code généré, capture ou binaire n'est versionné : le dépôt contient du code, des **masques de
signatures** (suites de classes d'instructions, jamais du code) et des adresses de référence.

## 1. Vue d'ensemble

```
tools/recipe_build.py identify|build|status|launch-info|clean <source> [--generic]
  identify   -> JSON : mode, XDK, fonctions D3D trouvées/manquantes (score), décalages lus, supported, reason
  build      -> recette si games/<TitleID>/ verrouille ce XEX, sinon mode générique :
     scaffold  analyse D3D (tools/d3d_match.py) -> profil et crochets générés (gen/), projet depuis games/_generic/
     codegen   boucle automatique (tools/codegen_loop.py) jusqu'à un codegen propre
     shaders   balayage binaire des fichiers du jeu et de l'image (games/_generic/tools/scan_shaders.py) + shaders manqués d'une partie
     compile   cmake + ninja ; stubs générés pour les imports du noyau inconnus
     bootstrap démarrage sous Xvfb + lavapipe : « ngpu_core on » et « first native frame presented »
```

Interface stable pour le Dashboard : mêmes sous-commandes, mêmes JSON et `progress.json` (étapes inchangées). Champs ajoutés :
`identify` : `mode` (`recipe`|`generic`), `supported`, `reason`, `xdk`, `static_libraries`, `d3d` (`found`, `missing`, `weak`, `offsets`),
`extra_modules`, `generic_supported`/`generic_reason` (pour un jeu en recette) ; `status` et `launch-info` : `mode`. Lot B2 : `d3d` reçoit
aussi `layout`, `optional_missing` et `offset_sources` (ajouts seulement). L'option `--generic` (ou `XD_GENERIC=1`) ignore la recette ;
`XD_MIN_DISK_GB` abaisse le seuil d'espace libre (essais).

## 2. `tools/d3d_match.py` : retrouver la XDK D3D par motif de code

Les fonctions D3D de la XDK sont liées statiquement dans le XEX ; ngpu_core les remplace par des crochets. Il faut leurs adresses et les
décalages du périphérique, qui changent d'une XDK à l'autre.

- **Jetons.** Chaque instruction PowerPC est réduite à un jeton *lâche* (classe d'opcode ; registres, immédiats et déplacements de branche
  masqués) et à un jeton *serré* (classe + registres ; immédiats masqués, sauf les paramètres de `rlwinm`/`rlwimi`).
- **Signatures** (`tools/d3d_signatures/*.json`, générées par `d3d_match.py learn` à partir de `references.toml`) : par fonction de référence,
  la suite des jetons (masque), les constantes caractéristiques (hachées en valeurs 16 bits), les imports du noyau appelés, des **champs** (quelle
  instruction charge/stocke tel décalage du périphérique : l'indice dans la fonction), des **relations** (`after_end` : commence juste après la fin de
  BeginVertices ; `after` : adresse plus haute ; `callee_of` : appelée par l'enveloppe).
- **Candidats** : index inversé de 5-grammes de jetons lâches sur les fonctions de `.pdata`, plus recherche exacte des 14 premiers jetons
  (fonctions sans entrée `.pdata`, fonctions minuscules : EndVertices fait 6 instructions).
- **Vérification** : alignement `difflib` des jetons ; score = moyenne des ratios lâche et serré ; champs lus **à travers l'alignement** (le
  décalage vient de l'instruction alignée dans la fonction trouvée, pas d'une table) ; contrôles croisés (imports du noyau atteints : `VdSwap`
  pour Swap ; constantes) ; relations ; unicité (une fonction ne sert qu'un rôle). Confiance = score pondéré par les contrôles et la marge sur le
  deuxième candidat.
- **Rôles requis** (`REQUIRED_ROLES`, 11) : DrawVertices, DrawIndexedVertices, Clear, Resolve, Swap (la fonction qui appelle `VdSwap`),
  CreateVertexShader/CreatePixelShader, VblankHandler, WaitFence, WaitDrain, WaitSecond. **Facultatifs** (`OPTIONAL_ROLES`, listés dans
  `optional_missing`) : BeginVertices, EndVertices, BeginTiling, GpuBeginShaderConstantF4 (absent de Skate). SetTexture, Set*ShaderConstantF et
  `setjmp`/`longjmp` du CRT (signature indépendante de la XDK) restent cherchés.
- **Décalages lus** : vertex_shader, pixel_shader, index_buffer, fence_ptr, fence_value, drain_ptr_word, second_word, fetch_base_slot/fetch_stride,
  vertex/pixel_constants_slot, reg_surface_info, reg_copy_dest_base.
- **Adresse du `D3DDevice`** : jamais figée ; la façade la capture à l'exécution (r3 du premier appel d'une méthode crochetée).
- **Verdict `supported`** : les 11 rôles requis avec confiance ≥ 0,85 ; les décalages `vertex_shader`, `pixel_shader`, `index_buffer` et les 4
  décalages du GPU nul lus dans le code du jeu. La disposition de l'ombre n'est plus comparée à une seule disposition : elle est recopiée dans
  `layout` et canonisée par `ngpu_core/src/device_layout.h` pour le noyau (`xenos_decode.h` ne lit toujours que l'ombre canonique). Une famille
  marquée `experimental` (`fusion-21256`) refuse le verdict pour la XDK de cette famille, sauf `XD_ALLOW_UNSUPPORTED=1` (diagnostic). `reason`
  liste ce qui manque ou échoue.

```
d3d_match.py match|analyze <xex|image.bin> [--exclude FAMILLE]     d3d_match.py compare <xex> <d3d_profile.toml>     d3d_match.py learn ...
```

Familles de signatures livrées : `skate-5632` (D3D 2.0.5632), `dash-6770` (2.0.6132), `crt-13604`, `crt-17559` (setjmp/longjmp), `xapi-21256`
(fibres XAPI de Trials Fusion), `fusion-21256` (Trials Fusion, D3D 2.0.21256 : **expérimentale**, voir §7.5). Lot B2 : les familles
`xapi-21256` et `fusion-21256` sont apprises ; les XDK 20158 (Dashboard 13604) et 17559 n'ont pas encore de famille D3D complète (§7.1).
Les adresses de référence sont celles de `games/45410813/d3d_profile.toml`, de `native_gpu/src/front6770.cpp` et des `dash_overrides.toml` 13604/17559.

## 3. Façade générique (`games/_generic/`)

`native/front.cpp` et `native/null_gpu.cpp` généralisent ceux de Skate : pilotés par `gen/d3d_profile.generated.h`, `d3d_hooks.generated.inc`,
`d3d_nullgpu_hooks.generated.inc` (un crochet par fonction retrouvée). Différences avec la façade de Skate : adresse du périphérique capturée ;
aucune image élargie (`ApplyGameAspect` supprimée), aucune liste de shaders d'interface ni de minicarte, plage de pitch de scène vide (échelle de rendu 1) ;
crochet de Swap généré (pas de plafond d'images, pas de pause ni de menus de Skate) ; enregistrement des shaders par crochet de création ; shader absent du cache : journal,
dessin sauté, microcode écrit (§5). `src/` : application minimale (`GenericApp`, aucun plugin GPU). Lot B2 : disposition de l'ombre par profil (`Layout()`), instantané canonique (`ngpu_core/src/device_layout.h`), constantes
`GpuBeginShaderConstantF4` superposées seulement si le rôle est sûr (absent de Skate), capture du `D3DDevice` validée par deux appels successifs.
Skate reste en recette (façade propre) : « façade générique + profil + hooks propres » est
l'étape suivante, non faite ici.

## 4. Boucle de codegen (`tools/codegen_loop.py`)

Reprise de la logique de `port_dashboard.py` (non refactorisé : ses fonctions sont couplées à son contexte ; le module est autonome) : cibles de branchements non
résolues, fonctions par pointeur (`find_ptr_funcs.py` + `filter_inside.py`), continuations de funclets, fonctions crochetées imposées, `setjmp`/`longjmp` par signature,
stubs `REX_STUB_RETURN` des imports inconnus à la compilation. Ajout : les candidats dont la référence est un **pointeur de données** sont acceptés même situés dans une
fonction générée (mesure sur Skate : 26 vrais pour 2 faux ; les références `lis/addi` du code restent filtrées : 70 vrais pour 1 045 faux).

**Limite connue : les fonctions appelées par pointeur et invisibles de l'analyse statique.** Le runtime s'arrête sur « Call to invalid or unregistered function ».
Le bootstrap écrit alors l'adresse dans `~/.local/share/xenondash/games/<TitleID>/missing_functions.toml` et demande de relancer la construction (le codegen la prend en compte).
Mesures : voir §7.

**Découverte statique (`tools/func_discover.py`, lot B2).** Candidats hors `.pdata` : mot de donnée pointant dans `.text` (nature `data`), adresse
construite par `lis` + `addi|ori` (`code`), et `data-in` (référence de donnée dans un corps `.pdata`, précédé d'un `b` ou `bl`). Filtre : pas de
mot nul ni d'adresse de code à cette adresse ; précédent terminal hors corps `.pdata` ; **jamais un thunk d'import du XEX** (table `img.thunks`).
Sans ce dernier filtre, Skate générique pur plantait au démarrage : 13 thunks (dont `0x8305822C`) devenaient des fonctions. Un `bl` vers
`KeTlsSetValue` appelait alors un STUB renvoyant 0 (`sub_8305822C STUB - returning 0x0` dans le journal) ; `sub_82E5B328` et `sub_82E5B468` se
rappelaient sans fin (gdb : pile saturée sur le fil principal). Mesure Skate après correction : 10 462 candidats (code 1947, data 8464, data-in 51) ;
précision 1945/1947, 8464/8464, 48/51 contre le codegen de la recette ; rappel 431/475 entrées de `functions.toml`. Le rejet automatique des faux
positifs et la reprise de boucle de `codegen_loop.py` sont écrits mais **non déclenchés** dans les essais de ce lot ; la boucle d'amorçage
(`XD_BOOT_LOOPS`, 12 par défaut) n'a pas été sollicitée non plus : aucune fonction non déclarée dans Skate ni dans Trials Fusion.

## 5. Shaders génériques et boucle « jouer → reconstruire »

`scan_shaders.py` balaie **tous** les fichiers du jeu (mmap) et l'image décodée à la recherche des conteneurs Xenos (magie `0x102A11xx`, en-tête de 0x24), y compris dans les archives non
compressées, puis ajoute les microcodes manqués (`~/.local/share/xenondash/games/<TitleID>/missing_shaders/*.bin`). XenosRecomp produit le cache compilé avec le jeu. Les archives
compressées (RefPack, zlib…) ne sont pas décompressées : leurs shaders arrivent par la boucle. À l'exécution, un shader créé par la XDK mais absent du cache est journalisé
(`not in the shader cache … draw skipped`), son conteneur est écrit dans `missing_shaders/` et le dessin est sauté ; **reconstruire** (`recipe_build.py build`) l'ajoute (seule l'étape
shaders et la compilation du cache sont refaites).

## 6. Diagnostic

`recipe_build.py identify <source>` : voir §1. Exemple de refus (Trials Fusion) en §7.

## 7. Résultats mesurés

Machine de développement chargée par d'autres constructions (charge 12 à 22 sur 12 cœurs), `--jobs 8`, clang 23, Xvfb + lavapipe. SDK `sdk/` et XenosRecomp non reconstruits.

### 7.1 Matcher : Skate (D3D 2.0.5632), Dashboard 6770 (2.0.6132), Dashboard 13604/17559, Trials Fusion (2.0.21256)

**Skate, toutes signatures** : `d3d_match.py compare` contre `games/45410813/d3d_profile.toml` : **29/29** identiques (16 adresses, 4 décalages du GPU nul, 9 décalages de l'ombre), mais ce
test est en partie circulaire (les signatures de Skate contiennent Skate). Les mesures non circulaires sont les validations croisées :

| Test (signatures exclues) | Résultat |
|---|---|
| Skate avec 6770 + CRT seulement | 17/29 : toutes les fonctions ayant un équivalent 6770 sont exactes (BeginVertices, EndVertices, Swap, SetVertexShader/SetPixelShader, VblankHandler, WaitFence/Drain/Second, enveloppes Create*) ; les 12 autres n'ont pas de référence 6770 (DrawVertices, DrawIndexedVertices, Clear, BeginTiling, Resolve, constructeurs `Create*Inner`, décalages index_buffer, fetch, reg_*) |
| 6770 avec Skate + CRT seulement | **14/14** rôles ayant un équivalent : BeginVertices `925CC6B8`, EndVertices `925CCB58`, CreateVertex/PixelShader `9212EE10`/`9212ED00`, SetVertex/PixelShader `9212E7E8`/`9212E4E8`, SetTexture `92123F20`, Set{Vertex,Pixel}ShaderConstantF `9212E2D8`/`9212E3C0`, Swap `92121950`, VblankHandler `92121210`, WaitFence/Drain/Second `9212A418`/`92129788`/`921296F0` ; décalages 12688, 12684, 10896, 10908, 60, 4, 120, 376 identiques |
| `setjmp`/`longjmp` : 13604 et 17559 avec les signatures de Skate | `9288D320`/`9288CEF0` et `9291C760`/`9291C330` (adresses connues des `dash_overrides.toml`) |
| `setjmp`/`longjmp` de Trials Fusion (jamais vu) | `82BC4F60`/`82BC5520` : **identiques** au `codegen.toml` de TrialsReVolution (trouvées là à la main) |

Remarques de lecture : le `Swap` de Skate (`82CCBE98`) est la fonction qui appelle `VdSwap` ; son équivalent 6770 est `92121950`, que l'entrée publique `92121EE8` (crochetée par `front6770.cpp`) appelle.
Les enveloppes `CreateVertexShader`/`CreatePixelShader` publiques du 6770 correspondent aux enveloppes de Skate (`82CC1078`/`82CC0EE8`), pas à ses constructeurs sur place (`82CC1038`/`82CC0EB0`, retrouvés par la relation
« appelé par l'enveloppe »). DrawVertices/DrawIndexedVertices/Clear/BeginTiling/Resolve de Skate trouvent des correspondants exacts dans le 6770 (`925CD180`, `925CD568`, `921289C8`…) dont l'adresse
de référence n'est pas connue : non vérifiés.

| XEX | XDK (D3D9) | rôles requis ≥ 0,85 | décalages lus (vertex_shader / index_buffer / fence_ptr) | verdict |
|---|---|---|---|---|
| Skate | 2.0.5632 | 16/16 | 12688 / 12428 / 10896 | supporté |
| Dashboard 6770 | 2.0.6132.4 | 15/16 (BeginTiling 0,77 : pas de référence 6770) | 12688 / 12428 / 10896 | refusé par le verdict (le Dashboard a sa façade) |
| Dashboard 13604 | 2.0.20158 | 6/16 | 12872 / 12612 / 10896 | refusé |
| Dashboard 17559 | 2.0.17559 | 5/16 | 13072 / 12812 / 11024 | refusé |
| Trials Fusion | 2.0.21256 | 3/16 | 13072 / 12812 / 11024 | refusé |

Le matcher trouve des candidats plausibles sur les XDK tardives (scores 0,7 à 0,97 : le code a changé), mais les décalages du périphérique ont bougé : `vertex_shader` 13072 au lieu de 12688, `pixel_shader` et
`fetch_*` non lus, `vertex_constants_slot` aberrant (-304 : instruction alignée à tort). Le noyau ne sait lire que l'ombre canonique ; les fonctions à 0,7 ne sont pas fiables.

### 7.2 Skate sans sa recette (`--generic`) : bout en bout

- `identify --generic` : mode `generic`, XDK 2.0.5632.0, 25 fonctions retrouvées, aucune manquante, supporté ; 8 s.
- Étape `scaffold` : analyse en < 1 s une fois le cache d'analyse fait (8 s à froid : index de n-grammes + alignements).
- **Codegen** : 4 passes (`rexglue codegen` ≈ 130 à 190 s chacune sous charge), 115 196 fonctions. La recette en compte 115 188 ; sans la liste de fonctions de Skate1Native, la boucle seule
  en trouve 115 061 : **132 de moins** (recette 481 entrées dont 349 retrouvées) et 5 de plus. Le premier démarrage s'arrête alors sur `Call to invalid or unregistered function at guest address 0x82BEB578`
  (fonction du port de Skate1Native, invisible à l'analyse statique). Pour valider le reste de la chaîne indépendamment de cette limite, la liste de fonctions du port a été copiée **à titre d'expérience**
  dans `missing_functions.toml` (cache utilisateur ; lecture de `games/45410813/functions.toml`, rien dans le dépôt) ; avec elle : 115 196 fonctions, aucune erreur.
- **Shaders** : 1re construction : 35 shaders (50 conteneurs trouvés par balayage : 26 dans l'image, 24 dans les 191 fichiers non compressés, 10 s pour 6,8 Go) ; les archives `.big` de Skate sont compressées (RefPack).
  Démarrage : `D3DDevice captured at 40002200` (**identique** à la constante de la recette), 142 shaders journalisés comme absents et écrits dans `missing_shaders/`. Reconstruction (étape shaders seule + cache) :
  177 shaders (la recette en a 348, issus de la décompression des archives) ; plus aucun shader manquant sur l'écran-titre.
- **Compilation** : 265 unités, 466 s ; bootstrap : « ngpu_core on » et « first native frame presented (1280x720) », 24,8 à 28,4 images/s sous lavapipe (la recette : 40 à 50 images/s sans autre charge ; machine chargée ici).
- Durées (sous charge) : codegen 777 s, shaders 9 s (le cache `missing_shaders` étant déjà là), compilation 466 s, bootstrap 43 s ; **total ≈ 21,6 min**. Construction par recette de Skate, mêmes conditions : 1056 s, clé de cache et bootstrap inchangés (`d86199b86b4ee9d2`).
- **Rendu** : écran-titre natif sous Xvfb + lavapipe (captures du scratchpad, non versionnées). `tools/framecmp.py` : générique contre recette **2,9 à 3,8/255** (PSNR 22 à 24 dB) ; recette contre recette **0,9 à 2,3/255** (PSNR 25 à 33 dB) : l'écran est animé
  (fondu du « Appuie sur START »), les deux images sont visuellement identiques mais pas au pixel près. Écart supplémentaire attendu : le générique n'a ni les hooks de Skate ni l'échelle propre à la scène.

### 7.3 Trials Fusion XBLA (jamais vu, XDK 2.0.21256)

Paquet STFS `Trials Fusion XBLA/` ouvert par `xcontainer.py` (extraction 15 s) ; XEX secondaire `UplayBrowser.xex` détecté et signalé.

- `identify` : mode générique, **`supported` = false**, raison : 13 fonctions D3D sous 0,85 (BeginVertices 0,70, EndVertices 0,80, DrawVertices 0,71, DrawIndexedVertices 0,69, Clear 0,85, BeginTiling 0,09, Resolve 0,57, Swap 0,68,
  CreatePixelShader 0,73, SetVertexShader 0,85, SetPixelShader 0,83, VblankHandler 0,80, WaitFence 0,79) et disposition de l'ombre différente (vertex_shader 13072/12688, index_buffer 12812/12428, pixel_shader et fetch non lus,
  vertex_constants_slot -304/120). Sans `XD_ALLOW_UNSUPPORTED`, la construction s'arrête là (message identique) : aucun repli sur l'émulation.
- Pour aller plus loin, essai de **diagnostic** (`XD_ALLOW_UNSUPPORTED=1`, qui n'ignore que le verdict) :
  - codegen générique : 6 passes, 58 694 fonctions ; sans aide, la boucle trouve 192 des 253 entrées des `functions.toml` + `missing_funcs.toml` de TrialsReVolution (**aucune de plus**) et le premier démarrage s'arrête sur
    `0x827A1C38` (« data ptr @0x82066F78, after a call » dans leur liste) ; liste de TrialsReVolution copiée en expérience dans le cache utilisateur ensuite ;
  - compilation : réussie (245 unités, ≈ 8 min), 56 shaders (26 manqués d'un premier démarrage, ajoutés par la boucle) ;
  - démarrage : le jeu tourne, `ngpu_core on`, crochets appelés, `D3DDevice captured at FB62C1D8` (adresse sans commune mesure avec 0x40002200 : le crochet qui a capturé r3 n'est probablement pas une méthode du périphérique, signe d'un faux
    positif), puis **aucune image native en 40 s**, sans erreur fatale.
- **Où ça bloque et ce qu'il faudrait** :
  1. **Signatures par famille de XDK** : références D3D pour 2.0.20158/21256 (Dashboard 13604, Trials Fusion) ; faute d'adresses de référence sûres, aucune signature de cette famille n'a été apprise (apprendre sur des candidats à 0,7 revient à valider des suppositions).
  2. **Ombre du périphérique** : disposition différente (vertex_shader 13072, index_buffer 12812, fence 11024/11036) ; ngpu_core ne lit qu'une ombre canonique : rendre `xenos_decode.h` paramétrable par profil
     (décalages, groupes de registres) ou synthétiser l'ombre depuis les crochets `Set*`.
  3. **Fibres XAPI** (`[rexcrt]` de TrialsReVolution : ConvertThreadToFiber…) : non détectées ni mappées par le mode générique.
  4. Fonctions par pointeur (61 de 253 manquantes), XEX secondaire, vidéo (Bink), modules DLC.

### 7.4 Lot B2 : Skate générique pur (sans liste copiée, sans recette)

- Premiers essais : `Unhandled guest access violation: write of guest 0x700CFFE0` juste après « ngpu_core on » (`skate_pure1`, puis sans les
  `data-in` : même plantage). Cause : les thunks d'import (§4), corrigée dans `func_discover.py`. gdb sur le binaire : SIGSEGV dans la récursion
  `sub_82E5B328` / `sub_82E5B468`.
- Construction corrigée (cache `b2thunk`, trois natures par défaut, `--jobs 8`, `XD_MIN_DISK_GB=3`) : **3 passes** de codegen (≈ 170 à 180 s
  chacune), 115 154 fonctions, 10 476 par pointeur dont 10 462 par découverte statique, 6 continuations, 0 rejet ; shaders 8 s (177 shaders, dont
  142 issus de parties précédentes) ; compilation 389 s ; amorçage 42 s ; **total 1031 s**.
- Journal du jeu : `D3DDevice captured at 40002200` (même adresse que la recette), `first native frame presented (1280x720)`, aucune violation
  d'accès. Capture : écran-titre de Skate (logo, « Appuie sur START pour continuer »), regardée à l'œil. Pas de comparaison au pixel près avec la
  recette dans ce lot.
- Non-régression : recette Skate, clé `d86199b86b4ee9d2` **inchangée**, codegen et shaders déjà en cache, construite en 62 s, amorçage OK.
- Dashboard 6770 natif (`cmake --build dashboards/2.0.6770.0/out/build/native -j8`) : compile et lie `dash` sans échec.

### 7.5 Lot B2 : Trials Fusion XBLA, essai de rendu

- `identify --generic` : XDK 2.0.21256.0. Avant le drapeau `experimental`, `supported` = true, mais la famille `fusion-21256` a été apprise sur
  Trials Fusion (contrôle circulaire ; sans elle, 3/11 rôles). Après : `supported` = false, `reason` = « XDK 2.0.21256.0 (D3D9) : signatures
  expérimentales (fusion-21256), rendu natif non validé ». Skate générique reste `supported` = true.
- Construction d'essai (cache `b2fusion`, `XD_DISPLAY=:93`, `XD_NGPU_TRACE=1`) : **5 passes** de codegen (58 674 fonctions, 6 165 par pointeur dont
  6 153 par découverte statique, 21 continuations), 750 s ; 56 shaders (26 issus de parties précédentes) ; compilation 331 s ; **total 1127 s**.
- Amorçage : `D3DDevice captured at 40852680`, `first native frame presented (1024x608)`, 600 échanges (`Swap`) tracés à 3 appels enregistrés
  chacun, 19 avertissements `draw without a bound shader in the device shadow (vs 00000000, ps 00000000) : skipped`. **Image entièrement noire**
  (`bootstrap.png`).
- Conclusion : le moteur présente des images, mais les dessins sont sautés faute de shader lié dans l'ombre du périphérique. La cause n'est pas
  établie. Piste de la passation (§5 de la note de travail) : shaders chargés hors de `SetVertexShader`/`SetPixelShader`, par une fonction du type
  `GpuLoadShaders`, non cherchée. Le rendu natif de Trials Fusion n'est donc pas validé, et la famille est marquée expérimentale.
- Non fait : passage des menus, fibres XAPI à l'exécution, Bink, XEX secondaire `UplayBrowser.xex` (seulement signalé).

## 8. Reste

- Trials Fusion : shaders absents de l'ombre à l'exécution (§7.5). Dashboard 13604 et 17559 : références Clear, VblankHandler (13604) et Swap
  (17559) manquantes ; ne pas baisser le seuil pour les faire passer. Fibres XAPI : famille `xapi-21256` apprise, pas encore appliquée à l'exécution.
- Découverte statique des fonctions par pointeur (les références `lis/addi` sont trop bruitées : 1 045 faux pour 70 vrais ; piste : analyse du flot, ou génération de tous les candidats avec repli à l'exécution).
  Aujourd'hui un démarrage s'arrête sur la première fonction non déclarée, que le bootstrap consigne dans `missing_functions.toml` ; chaque tour coûte un codegen (≈ 3 min) : **pas viable pour 130 fonctions**.
  Piste immédiate : rendre le « fonction non enregistrée » du runtime non fatal pour enregistrer toutes les adresses d'une partie, puis un seul codegen.
- Décompression des archives courantes (RefPack d'EA, zlib) pour récolter les shaders sans boucle ; reprise du cache de pipelines.
- Codegen incrémental (comparer le contenu des fichiers générés avant de les remplacer) pour que ninja ne recompile que les fichiers changés.
- Skate en « façade générique + profil + hooks propres » (non fait : la recette garde sa façade).
- Dashboard : `launch-info`/`status` donnent maintenant `mode` ; le Dashboard n'affiche pas encore le diagnostic `reason` d'un jeu refusé.
- Le cache est indexé par la révision de `d3d_match.py`, des signatures et de `codegen_loop.py` : toute modification de ces fichiers refait le codegen.
