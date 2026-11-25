# Ghidra : XAM et Dashboard Kinect 2.0.13604

But : lire une fonction en C décompilé à côté du code généré par ReXGlue, avec des noms partagés par toutes les
sessions (fichier de symboles versionné). Ghidra ne sert qu'à **comprendre** : le Dashboard reste recompilé et rendu
nativement, rien de Ghidra n'est exécuté.

## Prérequis

- Ghidra 12.2 (`/opt/ghidra`, ou `GHIDRA_INSTALL_DIR`) et Java 21 ;
- `tools/xexinfo/build/xexinfo` compilé (il produit l'image mémoire déchiffrée, la même que celle que ReXGlue recompile) ;
- le code généré de `dashboards/2.0.13604.0-xam` (compilation de la variante) : il donne les débuts de fonctions ;
- `xam.xex` et `dash.xex` dans le dossier de données de l'AppImage (`--data`, par défaut
  `~/.local/share/xenondash/2.0.13604.0-xam`).

## Commandes

```sh
python3 tools/ghidra/xd_ghidra.py import xam      # (re)crée le programme xam.img dans le projet et applique les noms
python3 tools/ghidra/xd_ghidra.py import dash
python3 tools/ghidra/xd_ghidra.py decompile xam sub_819BE610 XamFiber_Switch   # un .c par fonction
python3 tools/ghidra/xd_ghidra.py apply xam       # après modification de symbols/2.0.13604.0/xam.tsv
python3 tools/function_inventory.py xam --find XamFiber   # les mêmes noms dans l'inventaire
```

Le projet `xd13604.gpr`, les images et les sorties sont dans `work/ghidra/2.0.13604.0/` de la copie de travail
**principale** (un seul projet pour tous les worktrees ; Ghidra refuse un chemin qui contient `.claude`), ou dans
`XD_GHIDRA_WORK`. Ils ne vont jamais dans le dépôt : ils contiennent du code Microsoft. Interface graphique :
`ghidraRun`, File > Open Project, puis `xd13604.gpr` ; la fermer avant une commande (le projet est verrouillé).

## D'où viennent les noms

Par priorité croissante (`xd_ghidra.py entries <module>` écrit la liste sans lancer Ghidra) :

1. ReXGlue : tous les débuts de fonctions (`generated/<module>/codegen.partition.json`), nommés `sub_XXXXXXXX` ;
   Ghidra calcule les corps en suivant le flot ;
2. aides de sauvegarde et de restauration des registres (`__savegprlr_27`, `__restfpr_14`...), lues dans le code généré ;
3. imports : thunks (16 octets, `nop nop nop blr` dans l'image) et variables, d'après `xexinfo`, sans le préfixe `__imp__` ;
4. exports de XAM (1345) : table d'exports désignée par l'en-tête de sécurité du XEX, noms d'après les ordinaux du SDK
   (`third_party/xenondash-sdk/src/kernel/xam/export_table.inc`) ; vérifié : ordinal 772 `XamShowGuideUI` = 0x81898630,
   `XGetAVPack` = 0x818CAD20 ;
5. `symbols/2.0.13604.0/<module>.tsv` (versionné) : nos noms et commentaires (en-tête de fonction dans Ghidra).

Règles du fichier de symboles : noms issus de notre analyse uniquement (aucun symbole Microsoft) ; ne nommer que ce qui
est établi (code lu, mesure) ; une hypothèse se dit dans le commentaire ; nom vide = garder le nom automatique et
seulement ajouter un commentaire. Une adresse hors des sections de code devient une étiquette de donnée.

## Ce que fait l'application des entrées (`XdApplySymbols.java`)

Sans auto-analyse, trois corrections sont nécessaires pour un C juste (constatées sur le test ci-dessous) :

- **aides de registres « inline »** : sinon le décompilateur prend r3 pour la valeur de retour de `__savegprlr_27` et
  perd le premier argument (`iVar7 = __savegprlr_27();` au lieu de `param_1`) ;
- **thunks rendus ordinaires** : une fonction réduite à un saut devient un « thunk » qui affiche le nom de sa cible
  (`XGetAVPack` s'affichait `HalGetCurrentAVPack`) ; 232 dans XAM, 257 dans dash.xex ;
- **appels terminaux** : `b fonction` reçoit le flot CALL_RETURN (travail de l'analyseur « Shared Return Calls ») ;
  sinon l'appel disparaît du C (`XGetAVPack` se lisait `return;`) ; 8470 dans XAM, 18127 dans dash.xex.

Durées mesurées (2026-11-25) : import de XAM 35 s (16474 fonctions), de dash.xex 68 s (32877 fonctions), aucun échec.

## Test de la méthode : `sub_819BE610` (XAM)

C'est la boucle qui tournait à 100 % d'un cœur avant le correctif des fibres. Le C de Ghidra montre en une page ce que
le code généré cache dans 520 octets d'instructions : `XamFiberQueue_Pump(obj)` est une passe d'un répartiteur de
requêtes à fibres. Elle vide la file des requêtes prêtes (pointée par obj+0x10) par `XamFiberQueue_Resume`, qui reprend la
fibre de chaque requête par `XamFiber_SwitchTo` puis libère les requêtes terminées. Elle parcourt ensuite les requêtes en
attente (obj+0x34) par type : E/S XMsg, minuterie, rappel, E/S de fichier. Elle échange enfin les files sous la section
critique obj+0x18 et dort obj+0x78 ms quand il n'y a rien à faire. `XamFiber_SwitchTo` (ex-`sub_819BF160`) remappe la
pile de la fibre cible par `MmDoubleMapMemory`/`MmUnmapMemory`, **deux stubs du SDK** : ici aucune pile n'est remappée,
chaque fibre garde sa zone (à vérifier si XAM dépend de cet alias). Contre-épreuve sur dash.xex :
`Dash_BuildVideoModeList` décompilé redonne exactement le décodage manuel de la liste des résolutions (liaison +29, tables
0x9201D2D4 et 0x9201D330). Les noms et commentaires obtenus sont dans `symbols/2.0.13604.0/`.

## Limites

- Langage `PowerPC:BE:64:A2ALT-32addr` : Altivec oui, **VMX128 non** (extension du Xenon). Une fonction qui en contient
  s'arrête à la première instruction VMX128 (fin de corps ou `halt_baddata()` dans le C). L'extension XEXLoaderWV
  apporte un langage Xenon complet si besoin.
- Pas d'auto-analyse (lente et inutile pour les débuts de fonctions) : chaînes, références de données et types ne sont
  pas créés. L'analyse se lance au besoin dans l'interface, sur une sélection.
- Les thunks d'import (`nop nop nop blr` dans l'image) n'ont pas de prototype : le décompilateur ne voit ni arguments ni
  valeur de retour (`XGetAVPack` : `HalGetCurrentAVPack(); return;`). Prochaine étape utile : appliquer les prototypes
  des exports du SDK.
- Les adresses formées par `lis` apparaissent étendues en signe (`0xffffffff92979580`), langage 64 bits oblige.
