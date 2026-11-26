# Ghidra : XAM et Dashboard Kinect 2.0.13604

But : lire une fonction en C décompilé à côté du code généré par ReXGlue, avec des noms partagés par toutes les sessions (fichier de symboles versionné). Ghidra ne sert qu'à **comprendre** :
le Dashboard reste recompilé, rien de Ghidra n'est exécuté.

## Prérequis
Ghidra 12.2 (`/opt/ghidra` ou `GHIDRA_INSTALL_DIR`) et Java 21 ; `tools/xexinfo/build/xexinfo` (image déchiffrée, la même que celle recompilée) ; le code généré de `dashboards/2.0.13604.0-xam` (débuts de fonctions) ;
`xam.xex` et `dash.xex` dans le dossier de données de l'AppImage (`--data`, défaut `~/.local/share/xenondash/2.0.13604.0-xam`).

## Commandes
```sh
python3 tools/ghidra/xd_ghidra.py import xam|dash                       # (re)crée le programme et applique les noms
python3 tools/ghidra/xd_ghidra.py decompile xam sub_819BE610 XamFiber_Switch   # un .c par fonction
python3 tools/ghidra/xd_ghidra.py apply xam                             # après modification de symbols/2.0.13604.0/xam.tsv
python3 tools/function_inventory.py xam --find XamFiber                 # les mêmes noms dans l'inventaire
```

Projet `xd13604.gpr`, images et sorties : `work/ghidra/2.0.13604.0/` de la copie **principale** (un seul projet pour tous les worktrees ; Ghidra refuse un chemin contenant `.claude`) ou `XD_GHIDRA_WORK`.
Jamais dans le dépôt (code Microsoft). Interface : `ghidraRun`, File > Open Project ; la fermer avant une commande (projet verrouillé).

## D'où viennent les noms
Par priorité croissante (`xd_ghidra.py entries <module>` écrit la liste sans lancer Ghidra) :

1. ReXGlue : débuts de fonctions (`generated/<module>/codegen.partition.json`), `sub_XXXXXXXX` ;
2. aides de sauvegarde et restauration des registres (`__savegprlr_27`, `__restfpr_14`...) ;
3. imports : thunks (16 octets, `nop nop nop blr`) et variables, d'après `xexinfo`, sans `__imp__` ;
4. exports de XAM (1345) : table désignée par l'en-tête de sécurité du XEX, noms d'après les ordinaux du SDK (`export_table.inc`) ; vérifié : ordinal 772 `XamShowGuideUI` = `0x81898630`, `XGetAVPack` = `0x818CAD20` ;
5. `symbols/2.0.13604.0/<module>.tsv` (versionné) : nos noms et commentaires.

Règles de `symbols/` : noms issus de notre analyse (aucun symbole Microsoft), seulement ce qui est établi ; une hypothèse se dit dans le commentaire ; nom vide = nom automatique et commentaire seul.

## Ce que fait `XdApplySymbols.java` (sans auto-analyse)
- aides de registres « inline » (sinon r3 est pris pour la valeur de retour de `__savegprlr_27` et le premier argument est perdu) ;
- thunks rendus ordinaires (sinon `XGetAVPack` s'affiche `HalGetCurrentAVPack`) : 232 dans XAM, 257 dans `dash.xex` ;
- appels terminaux : `b fonction` reçoit le flot CALL_RETURN (sinon l'appel disparaît du C) : 8 470 dans XAM, 18 127 dans `dash.xex`.

Durées : import de XAM 35 s (16 474 fonctions), de `dash.xex` 68 s (32 877 fonctions). Méthode vérifiée sur `sub_819BE610` (XAM) : `XamFiberQueue_Pump`, passe d'un répartiteur de requêtes à fibres, boucle à 100 % d'un cœur
avant le correctif des fibres ; `XamFiber_SwitchTo` (ex-`sub_819BF160`) remappe la pile par `MmDoubleMapMemory`/`MmUnmapMemory`, deux stubs du SDK (aucune pile remappée ici). Contre-épreuve sur `dash.xex` :
`Dash_BuildVideoModeList` redonne le décodage manuel de la liste des résolutions (tables `0x9201D2D4`, `0x9201D330`).

## Limites
- Langage `PowerPC:BE:64:A2ALT-32addr` : Altivec oui, **VMX128 non** ; une fonction qui en contient s'arrête à la première instruction VMX128 (`halt_baddata()` dans le C). XEXLoaderWV apporte un langage Xenon complet.
- Pas d'auto-analyse : ni chaînes, ni références de données, ni types (à lancer dans l'interface sur une sélection).
- Les thunks d'import n'ont pas de prototype (`XGetAVPack` : `HalGetCurrentAVPack(); return;`). Les adresses formées par `lis` apparaissent étendues en signe.
