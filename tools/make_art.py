"""Generates VivoTube's original LiveArea art (icon, background, startup gate) as 8-bit
palette PNGs, which is what the Vita's LiveArea expects. Standard library only.

    python tools/make_art.py
"""
import math
import os
import struct
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TOP = (24, 44, 74)       # deep blue
BOTTOM = (16, 92, 104)   # teal
CORAL = (244, 106, 84)
WHITE = (246, 246, 250)


def lerp(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def mark_coverage(x, y, cx, cy, r):
    """The VivoTube mark: a ring with a bold chevron 'V' inside. Returns (ring, v) coverage."""
    d = math.hypot(x - cx, y - cy)
    ring = 1.0 if r * 0.80 <= d <= r else 0.0
    # Chevron: two thick strokes meeting at the bottom.
    def seg_dist(px, py, ax, ay, bx, by):
        dx, dy = bx - ax, by - ay
        t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
        return math.hypot(px - (ax + t * dx), py - (ay + t * dy))
    w = r * 0.13
    ax, ay = cx - r * 0.38, cy - r * 0.34
    bx, by = cx, cy + r * 0.36
    ex, ey = cx + r * 0.38, cy - r * 0.34
    v = 1.0 if min(seg_dist(x, y, ax, ay, bx, by), seg_dist(x, y, ex, ey, bx, by)) <= w else 0.0
    return ring, v


def render(w, h, marks, ss=3, rounded=0, extra=None):
    """Renders the background gradient with marks [(cx, cy, r)], supersampled ss x ss."""
    px = []
    for y in range(h):
        row = []
        for x in range(w):
            acc = [0.0, 0.0, 0.0]
            for sy in range(ss):
                for sx in range(ss):
                    fx = x + (sx + 0.5) / ss
                    fy = y + (sy + 0.5) / ss
                    t = (fx / w * 0.35 + fy / h * 0.65)
                    c = lerp(TOP, BOTTOM, t)
                    if extra:
                        c = extra(fx, fy, c)
                    for (cx, cy, r) in marks:
                        ring, v = mark_coverage(fx, fy, cx, cy, r)
                        if ring:
                            c = WHITE
                        if v:
                            c = CORAL
                    if rounded:
                        # Transparent corners are not allowed, so fade corners to the dark tone.
                        qx = min(fx, w - fx)
                        qy = min(fy, h - fy)
                        if qx < rounded and qy < rounded:
                            if math.hypot(rounded - qx, rounded - qy) > rounded:
                                c = (10, 18, 30)
                    acc[0] += c[0]
                    acc[1] += c[1]
                    acc[2] += c[2]
            n = ss * ss
            row.append((int(acc[0] / n + 0.5), int(acc[1] / n + 0.5), int(acc[2] / n + 0.5)))
        px.append(row)
    return px


def median_cut(colors, n):
    """colors: dict rgb -> count. Returns a palette of up to n colors."""
    boxes = [list(colors.items())]
    while len(boxes) < n:
        best, best_i, best_ch = -1, -1, 0
        for i, b in enumerate(boxes):
            if len(b) < 2:
                continue
            for ch in range(3):
                vals = [c[0][ch] for c in b]
                rng = max(vals) - min(vals)
                if rng > best:
                    best, best_i, best_ch = rng, i, ch
        if best_i < 0 or best == 0:
            break
        b = sorted(boxes.pop(best_i), key=lambda c: c[0][best_ch])
        total = sum(c[1] for c in b)
        acc, cut = 0, 1
        for k, c in enumerate(b):
            acc += c[1]
            if acc >= total / 2:
                cut = max(1, min(len(b) - 1, k + 1))
                break
        boxes.append(b[:cut])
        boxes.append(b[cut:])
    pal = []
    for b in boxes:
        tot = sum(c[1] for c in b)
        pal.append(tuple(int(sum(c[0][ch] * c[1] for c in b) / tot + 0.5) for ch in range(3)))
    return pal


def write_png_palette(path, px):
    h, w = len(px), len(px[0])
    counts = {}
    for row in px:
        for c in row:
            counts[c] = counts.get(c, 0) + 1
    pal = list(counts) if len(counts) <= 256 else median_cut(counts, 256)
    cache = {}

    def idx(c):
        if c in cache:
            return cache[c]
        best = min(range(len(pal)), key=lambda i: (pal[i][0] - c[0]) ** 2 + (pal[i][1] - c[1]) ** 2 + (pal[i][2] - c[2]) ** 2)
        cache[c] = best
        return best

    raw = bytearray()
    for row in px:
        raw.append(0)
        raw.extend(idx(c) for c in row)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    out = b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 3, 0, 0, 0))
    out += chunk(b"PLTE", b"".join(bytes(c) for c in pal))
    out += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    out += chunk(b"IEND", b"")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(out)
    print("wrote", os.path.relpath(path, ROOT), f"{w}x{h}", len(pal), "colors")


def stripes(fx, fy, c):
    # Soft diagonal light bands for the LiveArea background.
    k = math.sin((fx * 0.6 - fy) / 46.0)
    if k > 0.92:
        return lerp(c, (255, 255, 255), 0.06)
    return c


def main():
    sce = os.path.join(ROOT, "sce_sys")
    write_png_palette(os.path.join(sce, "icon0.png"), render(128, 128, [(64, 64, 44)], ss=4))
    write_png_palette(os.path.join(sce, "livearea", "contents", "startup.png"),
                      render(280, 158, [(140, 79, 52)], ss=3))
    write_png_palette(os.path.join(sce, "livearea", "contents", "bg.png"),
                      render(840, 500, [(640, 250, 120)], ss=2, extra=stripes))


if __name__ == "__main__":
    main()
