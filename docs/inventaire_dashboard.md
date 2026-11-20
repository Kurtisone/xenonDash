# Inventaire des fonctions du Dashboard 2.0.13604

Document de référence, **régénérable** : `python3 tools/function_inventory.py dash --summary`.
Les tables complètes (TSV/JSON) ne sont pas versionnées (3,1 Mo) ; la commande pour les produire est
en fin de page.

**32640 fonctions** réparties dans 129 fichiers générés.

## Pourquoi cet inventaire

Chercher une adresse à la main dans 32 640 fonctions ne tient pas. Cette table répond en une
commande aux questions qui reviennent : « qui appelle cette fonction ? », « celle-ci est-elle du
code Dashboard ou du runtime C ? », « quelles fonctions touchent le rendu ? ».

## Rôles

Certains rôles sont déduits des **imports noyau** (rendu, fichiers, réseau…) ; les autres sont
**structurels**, calculés sur le graphe d'appels — ce qui distingue le vrai code Dashboard du bruit
de compilation.

| rôle | fonctions | signification |
|---|---|---|
| `divers` | 20257 | code Dashboard qui n'appelle aucun import noyau connu (logique pure, arithmétique, structures) |
| `feuille-muette` | 6167 | ni import ni appel sortant : constante, donnée, ou fonction non atteinte |
| `appel-virtuel` | 2268 | dispatch par vtable (interfaces XUI, objets) — le `bctrl` est le point d'entrée |
| `petit-relais` | 1811 | thunk, getter, simple saut (≤ 12 lignes) |
| `threads` | 1461 | crée ou attend des threads, événements, sections critiques |
| `profil-utilisateur` | 230 | profils, contenus, joueurs (XamUser*, XamProfile*, XamContent*) |
| `interface-xam` | 141 | interfaces de XAM (XamShow*, Xui*, notifications) |
| `reseau` | 83 | pile réseau (Nic*, Mtpd*, Net*) |
| `rendu` | 55 | D3D/Vd (VdSwap, command buffers) |
| `crypto-cles` | 48 | clés, crypto (XeKeys*, XeCrypt*) |
| `fichiers` | 44 | entrées/sorties (Nt*File, XexLoad*) |
| `diagnostic` | 28 | journalisation, dumps, télémétrie |
| `memoire` | 19 | allocations invitées (NtAllocateVirtualMemory, Rtl*Heap) |
| `audio` | 15 | XAudio, voix |
| `entrees` | 7 | manettes et clavier |
| `runtime-compilateur` | 5 | routines insérées par le compilateur (__savegpr, memset inline) — pas du code Dashboard |
| `synchronisation` | 1 | attentes et verrous |

## Imports noyau les plus utilisés

Bonne carte des dépendances réelles du Dashboard :

| import | fonctions |
|---|---|
| `RtlLeaveCriticalSection` | 1251 |
| `RtlEnterCriticalSection` | 1235 |
| `RtlInitializeCriticalSection` | 147 |
| `ExGetXConfigSetting` | 126 |
| `_snwprintf` | 124 |
| `KeReleaseSpinLockFromRaisedIrql` | 57 |
| `KeAcquireSpinLockAtRaisedIrql` | 56 |
| `ExSetXConfigSetting` | 56 |
| `KfLowerIrql` | 55 |
| `KeRaiseIrqlToDpcLevel` | 54 |
| `RtlInitAnsiString` | 49 |
| `KeGetCurrentProcessType` | 47 |
| `XamUserGetSigninState` | 47 |
| `XMsgInProcessCall` | 47 |
| `XMsgCancelIORequest` | 46 |
| `ExReleaseReadWriteLock` | 44 |
| `XMsgStartIORequest` | 43 |
| `XNotifyGetNext` | 41 |
| `_snprintf` | 40 |
| `XamUserGetXUID` | 36 |
| `ExAcquireReadWriteLockShared` | 33 |
| `XMsgStartIORequestEx` | 31 |
| `NtClose` | 31 |
| `XamAlloc` | 30 |
| `XamFree` | 30 |
| `XamShowMessageBoxUI` | 28 |
| `KeSetEvent` | 28 |
| `XamEnumerate` | 27 |
| `KfAcquireSpinLock` | 26 |
| `KfReleaseSpinLock` | 26 |

## Points d'entrée connus (mesurés)

Fonctions identifiées au cours du travail, avec leur rôle réel. `appelants` = nombre de fonctions
du module qui l'appellent : **0 signifie qu'elle est atteinte par adresse** (thread, rappel, vtable).

| adresse | rôle | lignes | appelants | ce qu'elle fait |
|---|---|---|---|---|
| `9218eab0` | rendu | 950 | 10 | présentation du Dashboard : appelle VdSwap (entrée par sous_9218F138) |
| `9218f138` | divers | 166 | 11 | fonction d'image du Dashboard (appelée par sous_92172B48) |
| `92172b48` | divers | 287 | 4 | pilote d'image (boucle d'appel de l'image) |
| `92172670` | divers | 226 | 1 | pompe à images : appelle le pilote en boucle |
| `92173b80` | threads | 207 | 1 | entrée de la pompe (thread principal, lr entrant 0x92179BA0) |
| `921ae620` | threads | 291 | 0 | boucle de rendu/événements du Dashboard (créée par ExCreateThread, contexte 0x40004F18) |
| `92172a80` | divers | 35 | 0 | entrée de thread de la boucle d'image |
| `9217f108` | threads | 58 | 0 | trampoline de thread invitée (appelée par le runtime) |
| `92303780` | divers | 48 | 0 | entrée de thread (créée en 0x40351C80) |
| `9238b4d0` | divers | 259 | 0 | écriture invitée qui faute sous gdb (SIGBUS) |
| `9216e8a0` | divers | 98 | 1 | premier maillon qui ne rendait pas la main en mode XAM (verrou de liste) |
| `921b77c8` | divers | 219 | 2 | parcours de liste dans la chaîne d'initialisation (9 tours puis arrêt) |

## Fonctions les plus appelées, hors runtime

Utilitaires internes du Dashboard : c'est le socle sur lequel tout le reste s'appuie.

| adresse | rôle | lignes | appelants |
|---|---|---|---|
| `92495480` | divers | 42 | 899 |
| `924a2a48` | divers | 43 | 704 |
| `924b6738` | threads | 121 | 637 |
| `924b69a0` | threads | 85 | 637 |
| `9286dae8` | divers | 76 | 384 |
| `921bc6c8` | petit-relais | 11 | 372 |
| `921c18b0` | divers | 80 | 339 |
| `921cb2c0` | divers | 13 | 324 |
| `921b2070` | feuille-muette | 87 | 302 |
| `9286f520` | petit-relais | 9 | 293 |
| `92178828` | appel-virtuel | 47 | 293 |
| `92177d58` | feuille-muette | 12 | 273 |
| `92178c00` | petit-relais | 9 | 250 |
| `921cb298` | petit-relais | 9 | 246 |
| `9216e3f0` | divers | 43 | 246 |
| `921cefc0` | threads | 118 | 236 |
| `9217f288` | feuille-muette | 64 | 234 |
| `921b7940` | divers | 79 | 228 |
| `922e5760` | feuille-muette | 24 | 222 |
| `921b29d0` | feuille-muette | 63 | 220 |
| `92266d08` | divers | 52 | 205 |
| `92538e50` | feuille-muette | 94 | 196 |
| `927d29e0` | divers | 15 | 182 |
| `92880f18` | divers | 82 | 180 |
| `921cf088` | threads | 89 | 179 |

## Les plus grosses fonctions

Ce sont les sous-systèmes lourds (interfaces, contenu). Utile pour savoir où le Dashboard passe son
code, pas forcément où il passe son temps.

| adresse | rôle | lignes | appels sortants |
|---|---|---|---|
| `9279a2d0` | divers | 30818 | 1 |
| `928e9450` | divers | 25333 | 4 |
| `928c4a58` | divers | 15488 | 12 |
| `928f65c8` | divers | 12332 | 3 |
| `928b7038` | divers | 9999 | 9 |
| `92736e98` | divers | 7626 | 1 |
| `92800828` | divers | 6653 | 2 |
| `926c5058` | divers | 6437 | 9 |
| `928ab760` | divers | 6212 | 10 |
| `925d6aa8` | divers | 6209 | 11 |
| `928c1ae8` | divers | 6008 | 10 |
| `9273a9e8` | divers | 6006 | 1 |
| `925bac38` | divers | 5638 | 6 |
| `92691278` | divers | 5278 | 10 |
| `924f2058` | divers | 5225 | 45 |
| `926940b0` | divers | 5100 | 9 |
| `925d2578` | divers | 4966 | 15 |
| `928b2970` | divers | 4962 | 7 |
| `9270d738` | divers | 4905 | 16 |
| `928af908` | divers | 4864 | 8 |

## Autres modules

Même outil, mêmes colonnes :

| module | fonctions |
|---|---|
| `dash` | 32640 |
| `xam` | 16235 |
| `dash/hud` | 806 |
| `dash/gamerprofile` | 848 |
| `dash/ximecore` | 458 |
| `dash/signin` | 382 |
| `dash/createprofile` | 274 |
| `dash/vk` | 192 |
| `dash/deviceselector` | 169 |

## Utilisation

```sh
# vue d'ensemble, rôles et imports
python3 tools/function_inventory.py dash --summary

# une adresse (exacte ou préfixe de plage)
python3 tools/function_inventory.py dash --find 9218EAB0
python3 tools/function_inventory.py dash --find 921e

# toutes les fonctions d'un rôle
python3 tools/function_inventory.py dash --role rendu

# les plus structurantes
python3 tools/function_inventory.py dash --top 30          # par taille
python3 tools/function_inventory.py dash --top-calls 30   # par nombre d'appelants

# exports complets (non versionnés : 3,1 Mo)
python3 tools/function_inventory.py dash --tsv  > docs/inventaire/dash.tsv
python3 tools/function_inventory.py dash --json > docs/inventaire/dash.json
```

Le même outil fonctionne aussi pour `xam`, `dash/hud`, `dash/vk`, `dash/signin`,
`dash/createprofile`, `dash/gamerprofile`, `dash/deviceselector`, `dash/ximecore`.
