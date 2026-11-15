#!/usr/bin/env python3
"""Embarque la toolchain de compilation (clang, clang++, cmake, ninja) dans l'AppImage,
pour que « Play Game » puisse recompiler un jeu sans rien installer sur l'hote.

L'AppImage embarque deja le SDK (rexglue), XenosRecomp et dxc : il ne manquait que les
trois outils que MissingToolchain() cherche dans le PATH.

Usage interne par make_appimage.sh :
    python3 tools/appimage/embed_toolchain.py <sources> <dest>

  <sources> = racine de la toolchain sur la machine de build (ex. /usr)
  <dest>    = dossier a remplir dans l'AppDir (usr/share/xenondash/toolchain)

Le resultat est une arborescence autonome :
  toolchain/bin/{clang,clang++,cmake,ninja}
  toolchain/lib/...            (bibliotheques dont cmake/ninja ont besoin)
  toolchain/lib/clang/<ver>/   (en-tetes et runtime de clang)

Les binaires sont copies tels quels ; leurs dependances ELF sont resolues et copiees aussi,
pour ne pas dependre des bibliotheques de l'hote.
"""
import os
import pathlib
import shutil
import subprocess
import sys

# Binaires a embarquer, dans l'ordre de recherche (plusieurs emplacements possibles).
WANTED = {
    "clang": ["/usr/lib/llvm-20/bin/clang", "/usr/bin/clang"],
    "clang++": ["/usr/lib/llvm-20/bin/clang++", "/usr/bin/clang++"],
    "cmake": ["/usr/bin/cmake", "/usr/local/bin/cmake"],
    "ninja": ["/usr/bin/ninja", "/usr/local/bin/ninja"],
}
# Bibliotheques systeme a ne PAS copier (fournies par la distribution cible).
SKIP_LIBS = (
    "linux-vdso", "libc.so", "libm.so", "libpthread.so", "libdl.so", "librt.so",
    "ld-linux", "libgcc_s.so", "libstdc++.so",
)


def ldd_deps(binary: pathlib.Path):
    """Bibliotheques non systeme dont depend `binary` (sortie de ldd)."""
    try:
        out = subprocess.run(["ldd", str(binary)], capture_output=True, text=True, timeout=60).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    deps = []
    for line in out.splitlines():
        line = line.strip()
        if "=>" not in line:
            continue
        _, rhs = line.split("=>", 1)
        target = rhs.strip().split(" (")[0].strip()
        if not target or target == "not found":
            continue
        name = os.path.basename(target)
        if any(name.startswith(s) for s in SKIP_LIBS):
            continue
        deps.append(pathlib.Path(target))
    return deps


def copy_bin(src: pathlib.Path, dst: pathlib.Path):
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    dst.chmod(0o755)


def embed(sources_root: pathlib.Path, dest: pathlib.Path):
    bin_dir = dest / "bin"
    lib_dir = dest / "lib"
    bin_dir.mkdir(parents=True, exist_ok=True)
    lib_dir.mkdir(parents=True, exist_ok=True)

    found, missing = {}, []
    for name, candidates in WANTED.items():
        for c in candidates:
            p = pathlib.Path(c)
            if p.is_file():
                found[name] = p
                break
        else:
            missing.append(name)

    if missing:
        print(f"toolchain : introuvable -> {', '.join(missing)}", file=sys.stderr)
        return 1

    libs = set()
    for name, src in found.items():
        copy_bin(src, bin_dir / name)
        print(f"toolchain : {name} <- {src} ({src.stat().st_size // 1024} Ko)")
        for dep in ldd_deps(src):
            libs.add(dep)

    # En-tetes et runtime de clang : indispensables pour compiler (stdio.h, crtbegin.o...).
    clang_bin = found["clang"]
    llvm_root = clang_bin.parent.parent          # /usr/lib/llvm-20
    headers = llvm_root / "lib" / "clang"
    if headers.is_dir():
        dst = lib_dir / "clang"
        shutil.copytree(headers, dst, dirs_exist_ok=True, symlinks=True)
        print(f"toolchain : en-tetes clang <- {headers}")
    else:
        print(f"toolchain : en-tetes clang introuvables sous {headers}", file=sys.stderr)

    # Modules CMake (CMAKE_ROOT) : sans eux cmake refuse de demarrer ("Could not find CMAKE_ROOT").
    for share in sorted(pathlib.Path("/usr/share").glob("cmake-*")):
        if (share / "Modules").is_dir():
            shutil.copytree(share, dest / "share" / share.name, dirs_exist_ok=True, symlinks=True)
            print(f"toolchain : modules cmake <- {share}")
            break

    # gcc : clang a besoin des en-tetes C++ de la bibliotheque standard.
    for gcc_inc in ("/usr/include/c++", "/usr/include/x86_64-linux-gnu/c++"):
        p = pathlib.Path(gcc_inc)
        if p.is_dir():
            rel = p.relative_to("/usr/include")
            shutil.copytree(p, dest / "include" / rel, dirs_exist_ok=True, symlinks=True)
            print(f"toolchain : en-tetes C++ <- {p}")

    for dep in sorted(libs):
        copy_bin(dep, lib_dir / dep.name)
    print(f"toolchain : {len(libs)} bibliotheque(s) copiee(s)")

    total = sum(f.stat().st_size for f in dest.rglob("*") if f.is_file())
    print(f"toolchain : {total // (1024 * 1024)} Mo dans {dest}")
    return 0


def main():
    if len(sys.argv) != 3:
        sys.exit("usage : embed_toolchain.py <sources> <dest>")
    sys.exit(embed(pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])))


if __name__ == "__main__":
    main()
