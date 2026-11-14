# Index de la documentation

Pour **utiliser** XenonDash (installation, jeux, contrôles, dépannage) : le [README](../README.md). Tout ce qui suit s'adresse aux développeurs.

| Document | Contenu |
|---|---|
| [technique.md](technique.md) | Référence technique : mise en place depuis les sources, arborescence, portage d'une version de Dashboard, versions portées, détails NXE et Metro, AppImage, lancement des jeux, profil joueur, corrections, problèmes connus |
| [REPRISE.md](REPRISE.md) | Point d'entrée pour reprendre le travail (autre session ou autre personne) : objectif, règles, méthode, pièges, suite |
| [../PLAN.md](../PLAN.md) | Plan et décisions par phases (P0 à P8), état d'avancement |
| [xam_recompile.md](xam_recompile.md) | XAM 13604 recompilé et ses modules (HUD, clavier virtuel, connexion, création de profil) : paliers A à N4, blocages, procédures |
| [ngpu_core.md](ngpu_core.md) | Noyau de rendu natif (Vulkan) commun, façades D3D du Dashboard 6770, du Dashboard 13604 et du HUD de XAM, shaders, parité mesurée |
| [rendu_natif.md](rendu_natif.md) | Étude de faisabilité et jalons du rendu D3D natif (historique) |
| [recettes.md](recettes.md) | Recettes de jeux, moteur `tools/recipe_build.py`, cache, GPU nul |
| [recompilateur_generique.md](recompilateur_generique.md) | Recompilation d'un jeu sans recette : découverte des fonctions, fonctions D3D par motif, signatures, limites |
| [bibliotheque.md](bibliotheque.md) | Formats de jeux lus (dossier, XEX, ISO, STFS, GOD), inventaire du dossier de jeux, `games.auto.toml` |
| [nxe_jeux.md](nxe_jeux.md) | Parcours des jeux dans le Dashboard NXE : registre fusionné, Game Library, Play Game, progression, AppImage |
| [fusion_ports.md](fusion_ports.md) | Inventaire et fusion des ports de jeux existants (SDK, licences, rendu natif) |

Règle de rédaction : le README reste court et destiné aux utilisateurs ; tout détail technique va dans `docs/`.
