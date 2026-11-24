# État détaillé du XAM, du profil et du Guide (13604)

Extrait de `REPRISE.md` (sections déplacées le 2026-11-24, texte inchangé). Pour l état courant, voir REPRISE.md ; les mesures ici datent des 21 au 23 novembre sauf mention contraire.

## Profil (palier N4)
`DASH_PROFILE_MODE=xam` (activé par l'AppImage) : création de profil, connexion et parcours « aucun
profil » avec les vraies fenêtres Xbox. Pilote STFS minimal côté noyau, profil local hors ligne (XUID
aléatoire, clé locale tirée au hasard), aucune clé de console. Validé (banc, 2026-11-23) : création,
connexion par Sign In sans code d'accès, gamertag sur le XMB (`XamUserReadProfileSettings` synchronisé avec
le profil HLE, cd7f3b0). `dash_auto_profile` (défaut `false`) : sans profil, `XamUserGetName`/`GetGamerTag`
renvoient une chaîne vide avec succès. Les profils de test se suppriment dans `profiles/<XUID>` et
`Content/<XUID>`. Détail : `docs/xam_recompile.md`.

## État du front (Guide)
Chaîne complète établie et sondée (`[guidetrace]`, `[guideio]`, `[scene]`, `[uistate]`, `[app254]`) :
G → portier `sub_8189F0D0` accepte → message `0x20004117` à l'app 254 (via `sub_8187B960`) → le
répartiteur du système UI déroule `0x8000000B` → `0x80000007` (objet de requête 0x402E9B80, scène
en +4) → chargeur `sub_8189BE20` (case 20) → ouvreur `sub_818A21B0`.

- **Corrigé (76797c9)** : l'ouvreur exige `UIstate(0x81BFCAE8)+172≠0, +176≠0, +216==0`. `+176` est
  posé par `sub_8189F050(1)`, déclenché par le message **`0x80000012`** (« interface système ON »,
  traité en loc_81894F3C de `sub_81894D50` ; `0x80000011` = OFF) que personne n'émet chez nous.
  Correctif dans `ShowXamGuide` (`dash_xam_redirect.cpp`) : (a) appel DIRECT du gestionnaire
  `sub_81894D50(0x80000012)` via le répartiteur (l'envoi par la file, r3=0, n'est JAMAIS distribué
  par le répartiteur — mesuré) ; (b) pose de `+172=1` (« pipeline d'affichage prêt » : sur console,
  l'init vidéo D3D de XAM le pose — chaîne que le rendu natif ngpu remplace). Résultat : la scène
  charge (`r3=0`), l'état avance (+0=2, +56=1), au banc ET sur la machine de l'utilisateur.
  **RETIRÉ depuis** (mesure du 2026-11-22, commentaire de `ShowXamGuide`) : plus d'envoi de `0x80000012` à la main
  ni de `+172` forcé ; avec `XeKeysGetStatus` = 0x4, le fil du HUD de XAM (`sub_818A2F90` → `sub_818A2C98`) pose
  lui-même `+172`/`+176`. Ce paragraphe ne décrit plus le code actuel.
- **Présentation (ancienne impasse, 2026-11-23)** : l'utilisateur signale que le Guide s'affiche sur le Deck
  avec le build de la période (cause précise non relevée dans ce document ; les mesures ci-dessous datent du
  21 novembre). Ce qui était mesuré alors : la scène chargée ne prend pas le rendu ; le compositeur affiche
  toujours le hub. Mesuré et écarté : le rappel `[0x402E9B80+48]` n'est jamais écrit, mais la pompe
  UI (`sub_818905B0`) saute ces items (état 2, zéro `[trap]`) — normal ; forcer la cellule « app
  active » (0x817E4B64) à 254 ne change rien à l'affichage — ce n'est pas le sélecteur de rendu.
- **Piste suivante** : les notifications post-ouverture émises par l'ouvreur (`sub_818C54E0(255,1)`,
  `sub_8187E240(9,1)`, écriture `[0x81C0638C]=1`) et le chemin de présentation des apps UI de XAM
  (contextes +8=0x3002B000) — qui appelle VdSwap pour une scène XAM (au repos, seul le dash
  présente, `[swap] lr=0x9218ED74`) et sur quoi le compositeur base la bascule.

## Correctifs récents (une ligne chacun, détail dans le journal git et `docs/xam_13604_compilation.md`)
- **Trap non fatal** (SDK `function_dispatcher.cpp`, f2c7948) : un appel indirect invité vers 0
  (vtable jamais construite, ex. destructeur de la tâche Game Library `sub_922AA048`) avortait tout
  le processus — c'était le « freeze » de la sortie de Game Library. Journal borné + `r3=0`.
- **`NicGetLinkState`** : ne JAMAIS écrire à travers r4 (l'appelant réseau passe un r4 hérité non
  commité) — état « pas de lien » par r3=0 seulement.
- **`XeKeysGetStatus`** : remplir la sortie (`*r3 = 0x4` par défaut, `DASH_XEKEYS_STATUS=<hex>` pour tester) — laissée
  à 0, XAM ouvre la boîte modale du boot (« Game Error »/« Unrecognized Disc ») ; avec 0xC (valeur retail), le fil
  du HUD de XAM ne démarre pas et le Guide ne s'affiche jamais (mesure du 2026-11-22).
- **xam.xex rekeyé aux DEUX emplacements** (flash ET racine des données) : le chargeur résout
  `game:\xam.xex` vers la racine (le montage `game:` du noyau masque la flash) ; code 2 = hash de
  bloc invalide (clé de session fausse), PAS fichier absent.
- **`XamNotifyCreateListener`/`XN_SYS_UI`** : la diffusion hôte est passée en opt-in
  (`DASH_HUD_NOTIFY=1`) — la diffusion automatique reposait le bit « interface active » de XAM.
- **Import hôte** : `xex_rekey.py` sans dépendance obligatoire (AES cryptography → libcrypto → pur
  Python, équivalents octet pour octet) ; polices `.xtt` détectées dans `/home/deck/XenonDash/polices/`.
- **Disque supprimé entièrement** (68c0aa8, d0a352e, SDK d62f5dd) — voir Règles.
- Défauts SDK plus anciens corrigés en chemin : `AccessViolationCallback` refusait `kGuestVirtual`
  (7348c20) ; registre des modules XUI (`game:\flash\<nom>.xex` normalisé) ; `KeSetTimerEx` transmet
  le DPC ; chargement de XAM asynchrone.

## Imports à risque (mesurés le 2026-11-23, comportement inchangé)
- `XeKeysVerifyRSASignature` (TRUE en dur) : 5 appels au démarrage, tous des deux appelants connus
  (0x8185A4C4 et 0x818BF0C0). Restreindre aux deux appelants ne change rien au parcours actuel.
- `XexLoadExecutable` : 1 appel sur le chemin du XMB et du Guide (lr 0x818783AC). **Le nom lu est vide** :
  le hook lit probablement mal ses arguments. Ne pas le faire échouer avant de corriger la lecture.
- `XexStartExecutable` : 1 appel (lr 0x8187A028), entrée non nulle.
- Imports absents (renvoient 0) : aucun appel dans le scénario XMB / Guide / connexion.

