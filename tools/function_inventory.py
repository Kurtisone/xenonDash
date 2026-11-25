#!/usr/bin/env python3
"""Inventaire des fonctions d'un module recompilé (Dashboard 13604, XAM, HUD...).

Sert de fondation pour le travail sur le Dashboard : au lieu de chercher des adresses à la main
dans 33 000 fonctions, produit une table consultable avec, pour chaque fonction :

  adresse      adresse invitée (0x9216B010)
  fichier      fichier généré qui la contient (dash_recomp.42.cpp)
  lignes       taille du corps généré
  appels       nombre d'appels sortants (bl directs)
  appelants    nombre d'appels entrants (calculé sur tout le module)
  imports      imports noyau utilisés (__imp__Xxx) — souvent le meilleur indice d'usage
  role         classification grossière par mots-clés d'imports (rendu, fichiers, réseau, ...)
  commentaire  première ligne de commentaire du code généré, s'il y en a un
  nom          nom connu : fichier versionné symbols/<version>/<module>.tsv, puis entrées Ghidra générées par
               tools/ghidra/xd_ghidra.py (exports de XAM, imports, aides du compilateur), si elles existent

Usage :
  function_inventory.py <module>                 table lisible (triée par adresse)
  function_inventory.py <module> --tsv           tabulé, pour un tableur
  function_inventory.py <module> --json          JSON complet
  function_inventory.py <module> --role rendu    filtre par rôle
  function_inventory.py <module> --find 9218EAB0 recherche une adresse (ou une plage 0x9218xxxx)
  function_inventory.py <module> --find XamFiber recherche un nom (sous-chaîne, sans casse)
  function_inventory.py <module> --top 40        les plus gros (par lignes) ou --top-calls

`module` est un dossier sous dashboards/<version>/generated/ : dash, xam, dash/hud, dash/vk...
"""
import argparse
import json
import os
import re
import subprocess
import sys
from collections import Counter, defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Rôles : mots-clés d'imports -> rôle. Le premier qui correspond gagne ; l'ordre compte donc du plus
# spécifique au plus général.
ROLE_RULES = [
    ("rendu", ("Vd", "D3D", "VdSwap", "VdSetSystemCommandBuffer", "VdQuerySystemCommandBuffer",
               "VdGetSystemCommandBuffer", "VdInitializeRingBuffer", "VdEnableRingBufferRPtrWriteBack")),
    ("graphismes-sys", ("VdDisplayFatalError", "VdSetGraphicsInterruptCallback", "VdGetCurrentDisplayInformation")),
    ("entrees", ("XamInput", "XInput", "XamGetInputState", "Keystroke")),
    ("profil-utilisateur", ("XamUser", "XamProfile", "XamContent", "XamPlayer", "XamGamerCard")),
    ("interface-xam", ("XamShow", "XamUI", "Xui", "XamNotify", "XNotify")),
    ("fichiers", ("NtCreateFile", "NtOpenFile", "NtReadFile", "NtWriteFile", "NtQueryInformationFile",
                  "NtSetInformationFile", "NtClose", "XexLoadImage", "XexLoadExecutable", "IoCreateFile")),
    ("memoire", ("NtAllocateVirtualMemory", "NtFreeVirtualMemory", "RtlAllocateHeap", "RtlFreeHeap",
                 "NtQueryVirtualMemory", "MmAllocatePhysicalMemory", "MmFreePhysicalMemory")),
    ("threads", ("ExCreateThread", "KeWaitForSingleObject", "KeSetEvent", "KeInitializeEvent",
                 "KeResetEvent", "KeSetTimerEx", "NtWaitForMultipleObjects", "KeInsertQueueDpc",
                 "RtlInitializeCriticalSection", "RtlEnterCriticalSection", "RtlLeaveCriticalSection",
                 "ExRegisterTitleTerminateNotification", "KeGetCurrentProcessType", "KeTlsGetValue",
                 "KeTlsSetValue", "ExTerminateThread", "KeDelayExecutionThread", "ExCreateThread")),
    ("reseau", ("Nic", "Mtpd", "NetDll", "XNet", "WSA", "XamNetworking")),
    ("audio", ("XAudio", "XamVoice", "Audio")),
    ("stockage", ("Stfs", "SataCdRom", "XamContentCreate", "XamContentDelete", "Mu", "Usbd", "Usb")),
    ("diagnostic", ("DbgPrint", "Dump", "Debug", "Etx", "Profiler", "XProfiler")),
    ("crypto-cles", ("XeKeys", "XeCrypt", "XexTransformImageKey", "Sha", "Hmac", "Rc4")),
    ("synchronisation", ("KeSetEvent", "KeWait", "NtSignalAndWait", "RtlEnterCriticalSection")),
    ("divers", ()),
]

CALL_RE = re.compile(r'^DEFINE_REX_FUNC\((sub_[0-9A-Fa-f]+)\) \{', re.M)
BL_RE = re.compile(r'\t// bl 0x([0-9A-Fa-f]+)')
CALL_RE2 = re.compile(r'\t(sub_[0-9A-Fa-f]+)\(ctx, base\);')
IMPORT_RE = re.compile(r'__imp__(\w+)')
INDIRECT_RE = re.compile(r'REX_CALL_INDIRECT_FUNC')


def role_for(imports):
    for name, keys in ROLE_RULES:
        if not keys:
            return name
        for imp in imports:
            for k in keys:
                if imp.startswith(k):
                    return name
    return "divers"


def scan_module(mod_dir):
    """Lit tous les dash_recomp.*.cpp du dossier et renvoie la liste des fonctions."""
    funcs = {}
    files = sorted(f for f in os.listdir(mod_dir) if re.match(r'dash_recomp\.\d+\.cpp$', f))
    if not files:
        return funcs, []
    for fn in files:
        path = os.path.join(mod_dir, fn)
        text = open(path, encoding='utf-8', errors='replace').read()
        # découpe en fonctions
        marks = [(m.start(), m.group(1)) for m in CALL_RE.finditer(text)]
        marks.append((len(text), None))
        for idx in range(len(marks) - 1):
            start, name = marks[idx]
            end = marks[idx + 1][0]
            body = text[start:end]
            addr = name[4:].lower() if name else None
            calls = set(BL_RE.findall(body)) | set(x[4:].lower() for x in CALL_RE2.findall(body))
            imports = sorted(set(IMPORT_RE.findall(body)))
            comment = ""
            for line in body.split("\n")[1:6]:
                t = line.strip()
                if t.startswith("//") and "bl 0x" not in t:
                    comment = t.lstrip("/ ").strip()
                    break
            funcs[addr] = {
                "address": addr,
                "file": fn,
                "lines": body.count("\n"),
                "calls": len(calls),
                "callees": sorted(calls),
                "imports": imports,
                "indirect": len(INDIRECT_RE.findall(body)),
                "role": role_for(imports),
                "comment": comment[:120],
            }
    # Rôles structurels, calculés sur le graphe d'appels : une fonction sans appelant notable est
    # probablement une entrée de thread, un rappel, ou du code mort ; une fonction qui en appelle
    # beaucoup est un orchestreur. C'est plus parlant que « divers » pour 30 000 fonctions.
    # Routines du compilateur : sauvegarde/restauration de registres et memcpy/memset inlines. Elles
    # ont des centaines d'appelants et aucun import — les confondre avec du code Dashboard noierait
    # l'inventaire (mesure : 9217FB40 a 1150 appelants : c'est memcpy, voir symbols/2.0.13604.0/dash.tsv).
    COMPILER_RT = {"9217fb40", "9217f360", "92175e20", "92175e18", "9217f2d4", "9217f2d8",
                   "9217f2e4", "9217f2e8", "9217f2ec", "9217f334", "9217f33c", "9217f324",
                   "9217f228", "9217f318", "9217f320", "9217f328", "9217f330", "9217f338"}
    for addr, f in funcs.items():
        imps = f["imports"]
        if addr in COMPILER_RT:
            f["role"] = "runtime-compilateur"
            continue
        if not imps and f["calls"] == 0:
            f["role"] = "feuille-muette"     # ni import ni appel : constante, stub, ou non atteint
        elif f["indirect"] > 0 and f["calls"] <= 2:
            f["role"] = "appel-virtuel"      # dispatch par vtable (interfaces XUI, objets)
        elif f["lines"] <= 12 and f["calls"] <= 1 and not imps:
            f["role"] = "petit-relais"       # thunk, getter, saut
    # appelants : une fonction est appelée par celles qui la citent
    callers = defaultdict(set)
    for addr, f in funcs.items():
        for c in f["callees"]:
            if c in funcs:
                callers[c].add(addr)
    for addr, f in funcs.items():
        f["callers"] = len(callers.get(addr, ()))
    return funcs, files


def load_names(version, module):
    """Noms connus, clé = adresse en minuscules (comme scan_module) : entrées Ghidra générées (work/ghidra/<version>/
    <module>.entries.tsv, si présentes), puis le fichier versionné symbols/<version>/<module>.tsv, prioritaire."""
    base_version = version.removesuffix("-xam")
    mod = os.path.basename(module.rstrip("/"))
    names = {}
    work = os.environ.get("XD_GHIDRA_WORK")  # même dossier que tools/ghidra/xd_ghidra.py
    if not work:
        r = subprocess.run(["git", "-C", ROOT, "rev-parse", "--path-format=absolute", "--git-common-dir"],
                           capture_output=True, text=True)
        main = os.path.dirname(r.stdout.strip()) if r.returncode == 0 and r.stdout.strip() else ROOT
        work = os.path.join(main, "work", "ghidra", base_version)
    ent = os.path.join(work, mod + ".entries.tsv")
    if os.path.exists(ent):
        with open(ent, encoding="utf-8") as f:
            for line in f:
                p = line.rstrip("\n").split("\t")
                if not line.startswith("#") and len(p) >= 3 and not p[2].startswith("sub_"):
                    names[p[1].lower()] = p[2]
    sym = os.path.join(ROOT, "symbols", base_version, mod + ".tsv")
    if os.path.exists(sym):
        with open(sym, encoding="utf-8") as f:
            for line in f:
                p = line.rstrip("\n").split("\t")
                if line.startswith(("#", "adresse\t")) or len(p) < 2 or not p[1].strip():
                    continue
                names[p[0].strip().lower().removeprefix("0x").zfill(8)] = p[1].strip()
    return names


def main():
    ap = argparse.ArgumentParser(description="Inventaire des fonctions d'un module recompilé.")
    ap.add_argument("module", help="dossier sous dashboards/<version>/generated/ (dash, xam, dash/hud...)")
    ap.add_argument("--version", default="2.0.13604.0-xam", help="version du dashboard (défaut 2.0.13604.0-xam)")
    ap.add_argument("--tsv", action="store_true")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--role", default=None)
    ap.add_argument("--find", default=None, help="adresse exacte (9218EAB0), préfixe (9218) ou nom (sous-chaîne)")
    ap.add_argument("--top", type=int, default=None, help="N plus grosses fonctions")
    ap.add_argument("--top-calls", type=int, default=None, help="N fonctions aux plus d'appelants")
    ap.add_argument("--summary", action="store_true", help="seulement les statistiques")
    args = ap.parse_args()

    mod_dir = os.path.join(ROOT, "dashboards", args.version, "generated", args.module)
    if not os.path.isdir(mod_dir):
        sys.exit(f"dossier absent : {mod_dir}")
    funcs, files = scan_module(mod_dir)
    if not funcs:
        sys.exit(f"aucune fonction trouvée dans {mod_dir}")

    names = load_names(args.version, args.module)
    for f in funcs.values():
        f["name"] = names.get(f["address"], "")
    rows = list(funcs.values())
    total = len(rows)
    roles = Counter(f["role"] for f in rows)
    imports_all = Counter(i for f in rows for i in f["imports"])

    if args.find:
        pat = args.find.lower().removeprefix("0x")
        by_addr = [f for f in rows if f["address"].startswith(pat)] if re.fullmatch(r"[0-9a-f]+", pat) else []
        rows = by_addr or [f for f in rows if args.find.lower() in f["name"].lower()]
        if not rows:
            sys.exit(f"aucune fonction dont l'adresse commence par {args.find} ou dont le nom le contient")
    if args.role:
        rows = [f for f in rows if f["role"] == args.role]
        if not rows:
            sys.exit(f"aucune fonction de rôle {args.role} (rôles : {', '.join(sorted(roles))})")

    if args.top:
        rows = sorted(rows, key=lambda f: -f["lines"])[: args.top]
    elif args.top_calls:
        rows = sorted(rows, key=lambda f: -f["callers"])[: args.top_calls]
    else:
        rows = sorted(rows, key=lambda f: f["address"])

    if args.json:
        json.dump({"module": args.module, "version": args.version, "files": len(files),
                   "total": total, "roles": dict(roles), "functions": rows},
                  sys.stdout, indent=1)
        return
    if args.tsv:
        print("adresse\tnom\trole\tlignes\tappels\tappelants\tfichier\timports\tcommentaire")
        for f in rows:
            print(f'{f["address"]}\t{f["name"]}\t{f["role"]}\t{f["lines"]}\t{f["calls"]}\t{f["callers"]}\t'
                  f'{f["file"]}\t{",".join(f["imports"][:6])}\t{f["comment"]}')
        return

    print(f"module {args.module} ({args.version}) : {total} fonctions dans {len(files)} fichiers")
    if args.summary or not (args.find or args.role or args.top or args.top_calls):
        print("\nrôles :")
        for r, n in roles.most_common():
            print(f"  {r:22s} {n:6d}")
        print("\nimports noyau les plus utilisés :")
        for i, n in imports_all.most_common(25):
            print(f"  {i:44s} {n:5d}")
        if args.summary:
            return
        print("\n(pour la table complète : --tsv, --json, --find, --role, --top)")

    if args.find or args.role or args.top or args.top_calls:
        print(f"\n{len(rows)} fonction(s) :")
        print(f'{"adresse":10s} {"role":20s} {"lignes":>6s} {"appels":>6s} {"appelants":>9s}  {"fichier":22s} nom')
        for f in rows:
            print(f'{f["address"]:10s} {f["role"]:20s} {f["lines"]:6d} {f["calls"]:6d} '
                  f'{f["callers"]:7d}  {f["file"]:22s} {f["name"]}')


if __name__ == "__main__":
    main()
