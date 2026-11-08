"""Packs Twemoji PNGs into assets/emoji/emoji.pak for VivoTube's text renderer.

    python -I tools/make_emoji.py <twemoji>/assets/72x72 [--size 32] [--out assets/emoji/emoji.pak]

Get the images from https://github.com/jdecked/twemoji (graphics: CC-BY 4.0). Each file is named
after its code points ("1f468-200d-1f469.png"); the pack stores them resized, keyed by those code
points with U+FE0F (the emoji presentation selector) removed, since text may or may not carry it.
Skin-tone variants are skipped to keep the pack small.

Layout (little endian):
    "VTEMOJI1"  u32 count  u32 maxLen
    count x { u8 len, len x u32 code point, u32 offset, u32 size }   sorted by code points
    PNG data (offsets are from the start of the file)
"""
import argparse
import io
import os
import struct
import sys

from PIL import Image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src", help="directory of Twemoji PNGs (assets/72x72)")
    ap.add_argument("--size", type=int, default=32)
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                                  "assets", "emoji", "emoji.pak"))
    a = ap.parse_args()

    entries = {}
    for name in sorted(os.listdir(a.src)):
        if not name.endswith(".png"):
            continue
        try:
            cps = tuple(int(p, 16) for p in name[:-4].split("-"))
        except ValueError:
            continue
        # Skin-tone variants are left out (half the set): text with a tone modifier shows the
        # default-tone emoji. The app strips U+1F3FB..U+1F3FF the same way when matching.
        if any(0x1F3FB <= c <= 0x1F3FF for c in cps):
            continue
        key = tuple(c for c in cps if c != 0xFE0F)
        if not key or key in entries:
            continue
        with Image.open(os.path.join(a.src, name)) as im:
            im = im.convert("RGBA").resize((a.size, a.size), Image.LANCZOS)
            # A 256-colour palette with alpha is indistinguishable at this size and far smaller.
            im = im.quantize(colors=256, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE)
            buf = io.BytesIO()
            im.save(buf, "PNG", optimize=True)
        entries[key] = buf.getvalue()

    keys = sorted(entries)
    max_len = max(len(k) for k in keys)
    header = 8 + 4 + 4
    index = sum(1 + 4 * len(k) + 8 for k in keys)
    out = bytearray(b"VTEMOJI1" + struct.pack("<II", len(keys), max_len))
    offset = header + index
    blobs = []
    for k in keys:
        data = entries[k]
        out += struct.pack("<B", len(k)) + struct.pack("<%dI" % len(k), *k) + struct.pack("<II", offset, len(data))
        blobs.append(data)
        offset += len(data)
    for b in blobs:
        out += b
    with open(a.out, "wb") as f:
        f.write(out)
    print("wrote %s: %d emoji, %d bytes, longest sequence %d" % (a.out, len(keys), len(out), max_len))


if __name__ == "__main__":
    sys.exit(main())
