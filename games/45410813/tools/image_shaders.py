# SPDX-License-Identifier: GPL-3.0-or-later
# Repris de Skate1Native (Kurtisone/Skate1Recompiled, GPL-3.0, commit 81805f51), recette XenonDash 45410813.
"""Copy the shader containers compiled into the executable (the post-processing
shaders) out of its decoded image (rexglue dump-xex):
image_shaders.py <image dir or file> <out dir>"""
import glob, os, struct, sys

BASE = 0x82000000


def main():
    source, out = sys.argv[1], sys.argv[2]
    paths = [source] if os.path.isfile(source) else glob.glob(os.path.join(source, "**", "*"), recursive=True)
    paths = [p for p in paths if os.path.isfile(p)]
    image = open(max(paths, key=os.path.getsize), "rb").read()
    count = 0
    for offset in range(0, len(image) - 0x24, 4):
        magic = struct.unpack_from(">I", image, offset)[0]
        if magic & 0xFFFFFFFE != 0x102A1100:
            continue
        virtual, physical, reserved, header = struct.unpack_from(">IIII", image, offset + 4)
        size = virtual + physical
        if reserved or header != 0x24 or not size or size > len(image) - offset:
            continue
        kind = "vs" if magic & 1 else "ps"
        with open(os.path.join(out, f"image_{BASE + offset:08X}_{kind}.bin"), "wb") as f:
            f.write(image[offset:offset + size])
        count += 1
    print(f"{count} shader containers in the image")


if __name__ == "__main__":
    main()
