# Bibliothèque de jeux : formats et inventaire

Date : 2026-11-12. L'utilisateur dépose ses jeux, sous n'importe quel format, dans un dossier ; XenonDash les identifie et le Dashboard NXE
(Games Library > My Games) les voit. Deux outils Python sans dépendance obligatoire : `tools/xcontainer.py` (lecture des formats) et
`tools/game_library.py` (inventaire). Aucun fichier de jeu n'est copié ni modifié ; aucun fichier de jeu ou de Microsoft n'est versionné.

## 1. Formats pris en charge (reconnus par le contenu, pas par l'extension)

| Format | Reconnaissance | Remarques |
|---|---|---|
| `dir` | dossier contenant `default.xex` (casse libre) | rien à extraire : le dossier est déjà utilisable |
| `xex` | magie `XEX2` | rien à extraire |
| `xdvdfs` | `MICROSOFT*XBOX*MEDIA` au secteur 32 à l'un des décalages 0, `0x0FD90000` (XGD2), `0x02080000` (XGD3), `0x18300000` (XGD1), puis `0xFB20`, `0x20600` (Xbox d'origine) | lecture ciblée (en-tête, répertoires, fichiers) : une ISO n'est jamais lue en entier |
| `stfs` | magie `LIVE`, `PIRS` ou `CON ` et descripteur de volume STFS | XBLA, démos, mises à jour, DLC. Métadonnées : TitleID, type de contenu, nom UTF-16BE, miniatures PNG |
| `god` | en-tête `LIVE`/`PIRS` dont le descripteur de volume est SVOD (octet `0x3A9` = 1) et dossier `<en-tête>.data/Data0000...` | flux XDVDFS reconstruit en sautant les blocs de hachage |
| `tree` | dossier sans `default.xex` qui contient des paquets (`<TitleID>/<type>/<paquet>`) | parcours récursif ; `list` donne les paquets |

STFS : une table de hachage tous les 170 blocs de données (L0), tous les 28 900 (L1), tous les 4 913 000 (L2) ; chaque table occupe 1 bloc
pour un paquet en lecture seule (LIVE/PIRS, bit 0 de l'octet `0x37B`) ou 2 blocs (CON). Les fichiers sont lus bloc par bloc, en suivant la
chaîne de la table L0 quand ils ne sont pas consécutifs. Noms refusés à l'extraction : chemins absolus ou contenant `..`.

SVOD : une partie (`DataNNNN`) = 1 bloc de hachage maître + 203 sous-parties de (1 bloc de hachage + 204 blocs de données), blocs de 4096 octets,
soit 41 412 blocs de données par partie. Le volume XDVDFS est cherché au secteur 32 du flux reconstruit, aux décalages `0x2000`, `0x1000` puis la liste ISO.

Identité du jeu : en-tête XEX2 en clair (TitleID, MediaID, version, version de base, numéro de disque, drapeaux de module). Nom et icône :
ressource XDBF du XEX (ressource nommée par le TitleID ; nom = chaîne `0x8000` de la langue par défaut, icône = image `0x8000`). L'image du XEX est
décodée par `rexglue dump-xex` (`$XD_SDK/bin/rexglue`, défaut `sdk/`, ou `$XD_REXGLUE`), à défaut par `tools/xexinfo/build/xexinfo` (`$XD_XEXINFO`).
Sans décodeur, repli sur l'en-tête STFS/GOD (nom, miniature de titre) ; sinon le nom est `Titre <TitleID>` et il n'y a pas d'icône.

## 2. `tools/xcontainer.py`

```
xcontainer.py info <chemin> [--fast]   JSON : format, métadonnées, xex (TitleID, MediaID, version...), executable, title (nom, taille d'icône, origine)
xcontainer.py list <chemin> [--json]   fichiers du conteneur (taille, chemin)
xcontainer.py cat <chemin> <fichier>   écrit le fichier sur stdout (casse libre)
xcontainer.py extract <chemin> <dest>  extraction complète ; progression sur stderr ; stdout = dossier contenant default.xex
```

`extract` est idempotente : le marqueur `<dest>/.extracted.ok` (signature de la source) évite de refaire le travail ; un fichier déjà extrait
à la bonne taille est conservé, l'écriture passe par `<fichier>.part`. Pour `dir` et `xex` il n'y a rien à extraire : la commande affiche le
dossier source et ne crée rien dans `<dest>`. Code de sortie 1 et message sur stderr en cas d'erreur (format inconnu, fichier corrompu).

API Python : `detect(path)` (`'dir'|'xex'|'xdvdfs'|'stfs'|'god'|'tree'|None`, jamais d'exception), `open(path)` (`FormatError` si illisible) avec
`list()` (liste de `{name, size}`), `read(nom)`, `iter_file(nom)`, `extract(dest, progress=None)` (`progress(octets_faits, total, nom)`), `metadata()`,
`find_xex()`, `xex_info()`, `title_data()` ; `info(path)` renvoie le dictionnaire de la CLI.

## 3. `tools/game_library.py`

```
game_library.py init [--dir D]                       crée le dossier (défaut $XENONDASH_GAMES_DIR, sinon ~/XenonDash/Games) et LISEZMOI.txt
game_library.py scan [--dir D] [--out O] [--force] [--json]
game_library.py list [--out O] [--json]
```

Sortie : `${XDG_DATA_HOME:-~/.local/share}/xenondash/library/` (ou `--out`).

Parcours : récursif, profondeur 8, liens symboliques suivis (boucles détectées par chemin réel), fichiers cachés ignorés. Un dossier avec `default.xex` est un jeu
(on n'y descend pas) ; le dossier `<en-tête>.data` d'un GOD est lu via son en-tête. **Un jeu = un TitleID** : si plusieurs sources portent le même, la
version la plus haute l'emporte, puis le format (`dir`, `xex`, `xdvdfs`/`god`, `stfs`) ; les autres sont dans `alternates`. Classées « pas un jeu » (signalées dans
`not_games`, absentes de `games.auto.toml`) : sauvegardes, DLC, profils, thèmes, mises à jour (type installeur `0xB0000`, `default.xexp`), paquet sans `default.xex`,
jeu Xbox d'origine (`default.xbe`), module ou DLL XEX, TitleID `FFFExxxx`. Un fichier illisible va dans `errors`, un fichier non reconnu dans `unrecognized` ; ni l'un ni l'autre
ne fait échouer le scan.

Cache incrémental : chaque source a une signature (chemin, taille, mtime) ; sans changement, elle n'est pas rouverte (scan de 144 sources : 1,3 s à froid, 0,14 s en cache).

### `games.auto.toml`

Même format que `games.toml` (voir `games.example.toml`, `LoadRegistry` de `common/src/dash_launch.cpp`), chemins absolus :

```toml
library = true

[game.45410813]
name = "skate."
recipe = "/chemin/absolu/vers/la/source"   # dossier, default.xex, ISO, paquet STFS ou en-tête GOD
type = "arcade"                            # arcade (défaut), demo (type 0x80000), xna (0xE0000)
icon = "/chemin/absolu/icons/45410813.png"
format = "dir"                             # informatif : dir | xex | xdvdfs | stfs | god
media_id = "21D6D331"                      # informatif
version = "0.0.0.3"                        # informatif
```

`recipe` est donc une source quelconque : `tools/recipe_build.py` doit appeler `xcontainer.py extract <source> <dossier>` pour les ISO, STFS et GOD.

### `library.json`

`schema` (1), `scanned_at`, `scan_seconds`, `games_dir`, `out_dir`, `sources_reused`, `sources_total`,
`games` (par TitleID : `title_id`, `name`, `name_origin` = `xex` | `en-tête` | `TitleID`, `format`, `source`, `type`, `content_type`, `media_id`, `version`,
`base_version`, `disc_number`, `icon`, `icon_bytes`, `icon_oversize`, `alternates`), `not_games`, `errors`, `unrecognized`, et `sources` (une entrée par
fichier ou dossier vu : `kind` = `game` | `not_game` | `unknown` | `error`, `sig` = `[chemin, taille, mtime_ns]` des fichiers qui signent la source, `kind_hint`) qui sert de cache.

`icons/<TitleID>.png` : l'icône du jeu, copiée telle quelle (`icon_oversize` vrai au-delà de 16 Ko : aucun ré-encodage, pas de bibliothèque d'image). `icons/.cache/` : copies par source pour le cache.

## 4. Tests

`python3 -m unittest discover -s tools/tests -p 'test_library_*.py'` (variable `XD_TEST_TMP` pour le dossier temporaire). Les tests sur fichiers réels
(Skate, paquet Trials Fusion, firmware) sont ignorés si les fichiers manquent ; les ISO, GOD et paquets STFS synthétiques sont fabriqués par
`tools/tests/test_library_formats.py` (aucun fichier de jeu dans le dépôt).

## 5. Limites

- ISO, GOD et STFS CON : validés uniquement sur des fichiers synthétiques fabriqués avec la même compréhension du format (pas d'image réelle disponible) ; décalage SVOD
  `0x2000` (disposition « Enhanced GDF ») non vérifié. STFS LIVE/PIRS : validé sur un paquet réel (chaque bloc de données contrôlé par son SHA-1) ; CON en lecture-écriture
  (tables doubles, choix de la copie active pour les fichiers non consécutifs) non validé.
- Nom et icône issus du XEX : demandent le SDK (`rexglue`) ; l'AppImage doit l'embarquer ou donner `XD_REXGLUE`.
- Pas de déchiffrement ni de lecture d'ISO chiffrées au-delà de la structure XDVDFS ; pas de multi-disques (le jeu est listé une fois, autres disques dans `alternates`).
- Les contenus Xbox d'origine ne sont pas lancés ; les ISO « Xbox d'origine » sont signalées.
- Les ISO fragmentées en plusieurs fichiers (`.iso.0`) ne sont pas assemblées.
- Les dossiers de jeu sans `default.xex` à la racine ne sont pas des jeux ; un `.xex` nu seul référence son dossier (les données du jeu doivent y être).
