# Index de la documentation

Pour **utiliser** XenonDash (installation, jeux, contrôles, dépannage) : le [README](../README.md). Tout ce qui suit s'adresse aux développeurs.

| Document | Contenu |
|---|---|
| [technique.md](technique.md) | Référence technique : mise en place depuis les sources, arborescence, portage d'une version de Dashboard, versions portées, détails Kinect et Metro, AppImage, lancement des jeux, profil joueur, corrections, problèmes connus |
| [REPRISE.md](REPRISE.md) | **Point d'entrée pour reprendre le travail** : état du front (Guide), règles, méthode, pièges, suite — lire en premier |
| [xam_13604_compilation.md](xam_13604_compilation.md) | Journal détaillé de la variante -xam (XAM 13604 recompilé, Guide, boîtes, entrées, disque) : sections datées 2026-11-20/21 |
| [../PLAN.md](../PLAN.md) | Plan et décisions par phases (P0 à P8), état d'avancement |
| [xam_recompile.md](xam_recompile.md) | XAM 13604 recompilé et ses modules (HUD, clavier virtuel, connexion, création de profil) : paliers A à N4, blocages, procédures |
| [ngpu_core.md](ngpu_core.md) | Noyau de rendu natif (Vulkan) commun, façades D3D du Dashboard 6770, du Dashboard 13604 et du HUD de XAM, shaders, parité mesurée |
| [rendu_natif.md](rendu_natif.md) | Étude de faisabilité et jalons du rendu D3D natif (historique) |
| [recettes.md](recettes.md) | Recettes de jeux, moteur `tools/recipe_build.py`, cache, GPU nul |
| [recompilateur_generique.md](recompilateur_generique.md) | Recompilation d'un jeu sans recette : découverte des fonctions, fonctions D3D par motif, signatures, limites |
| [bibliotheque.md](bibliotheque.md) | Formats de jeux lus (dossier, XEX, ISO, STFS, GOD), inventaire du dossier de jeux, `games.auto.toml` |
| [nxe_jeux.md](nxe_jeux.md) | Parcours des jeux dans le Dashboard Kinect (2.0.13604) : registre fusionné, Game Library, Play Game, progression, AppImage |
| [fusion_ports.md](fusion_ports.md) | Inventaire et fusion des ports de jeux existants (SDK, licences, rendu natif) |

## Interfaces du Dashboard Xbox 360 (datation)

Les quatre interfaces portent des noms d'usage communautaires ; les confondre fausse la documentation
(le Dashboard 2.0.13604 est un **Kinect**, pas un NXE).

| Interface | Versions | Remarque |
|---|---|---|
| Blades | 2.0.1888 – 2.0.6717 (+ 2.0.6770) | interface d'origine (2005–2008) |
| **NXE** (New Xbox Experience) | **2.0.7357 – 2.0.9199** | avatars, Games on Demand (2008–2010) |
| **Kinect** | **2.0.12611 – 2.0.13604** | Xbox 360 S et Kinect, tuiles, base du style Metro (2010–2011) |
| Metro | 2.0.14699 – 2.0.17559 | Metro v1 puis v2 (2011–2019) |

Dans ce dépôt, `dashboards/2.0.13604.0` porte donc le Dashboard **Kinect**. Aucune version NXE
proprement dite (7357–9199) n'est portée à ce jour ; le fichier `nxe_jeux.md` garde son nom
(référencé dans le code) mais décrit bien la 13604.

Règle de rédaction : le README reste court et destiné aux utilisateurs ; tout détail technique va dans `docs/`.
