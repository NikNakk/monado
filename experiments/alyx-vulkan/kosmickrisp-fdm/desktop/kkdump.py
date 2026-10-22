#!/usr/bin/env python3
"""Convert KosmicKrisp KK_DUMP_RESOLVE_DIR dumps to PNG and compare them.

usage:
  kkdump.py png DUMP [OUT_PREFIX]         write one PNG per layer, side by side
  kkdump.py stats DUMP...                 mean luminance per layer, eye difference
  kkdump.py diff A B                      per-layer mean absolute difference
"""
import struct
import sys
import zlib

VK_FORMAT_B10G11R11_UFLOAT_PACK32 = 122
VK_FORMAT_R16G16B16A16_SFLOAT = 97
VK_FORMAT_R8G8B8A8_UNORM = 37
VK_FORMAT_B8G8R8A8_UNORM = 44
VK_FORMAT_D32_SFLOAT = 126


def uf(bits, mant):
    e = bits >> mant
    m = bits & ((1 << mant) - 1)
    if e == 0:
        return m / (1 << mant) * 2.0 ** -14
    if e == 31:
        return 65504.0
    return (1 + m / (1 << mant)) * 2.0 ** (e - 15)


def load(path):
    with open(path, "rb") as f:
        header = f.readline().split()
        assert header[0] == b"KKDUMP", path
        w, h, layers, bpp, fmt = (int(x) for x in header[1:6])
        data = f.read()
    return w, h, layers, bpp, fmt, data


def pixels(w, h, layers, bpp, fmt, data, step=1):
    """Yield (layer, y, x, r, g, b) as floats."""
    lut = None
    if fmt == VK_FORMAT_B10G11R11_UFLOAT_PACK32:
        lut11 = [uf(i, 6) for i in range(1 << 11)]
        lut10 = [uf(i, 5) for i in range(1 << 10)]
    for l in range(layers):
        base = l * w * h * bpp
        for y in range(0, h, step):
            row = base + y * w * bpp
            for x in range(0, w, step):
                o = row + x * bpp
                if fmt == VK_FORMAT_B10G11R11_UFLOAT_PACK32:
                    v = struct.unpack_from("<I", data, o)[0]
                    r, g, b = lut11[v & 0x7FF], lut11[(v >> 11) & 0x7FF], lut10[v >> 22]
                elif fmt == VK_FORMAT_D32_SFLOAT:
                    r = g = b = struct.unpack_from("<f", data, o)[0]
                elif fmt == VK_FORMAT_R16G16B16A16_SFLOAT:
                    r, g, b = struct.unpack_from("<eee", data, o)
                elif fmt == VK_FORMAT_B8G8R8A8_UNORM:
                    b, g, r = (c / 255 for c in data[o:o + 3])
                else:
                    r, g, b = (c / 255 for c in data[o:o + 3])
                yield l, y, x, r, g, b


def tonemap(v):
    v = v / (1.0 + v)
    v = max(0.0, min(1.0, v)) ** (1 / 2.2)
    return int(v * 255 + 0.5)


def write_png(path, width, height, rows):
    raw = b"".join(b"\x00" + bytes(r) for r in rows)
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def cmd_png(path, out=None):
    w, h, layers, bpp, fmt, data = load(path)
    out = out or path.rsplit(".", 1)[0]
    rows = [bytearray(w * layers * 3) for _ in range(h)]
    for l, y, x, r, g, b in pixels(w, h, layers, bpp, fmt, data):
        o = (l * w + x) * 3
        rows[y][o:o + 3] = bytes((tonemap(r), tonemap(g), tonemap(b)))
    write_png(out + ".png", w * layers, h, rows)
    print(out + ".png")


def layer_lum(path, step=2):
    w, h, layers, bpp, fmt, data = load(path)
    sums = [0.0] * layers
    grids = [dict() for _ in range(layers)]
    n = 0
    for l, y, x, r, g, b in pixels(w, h, layers, bpp, fmt, data, step):
        lum = 0.2126 * r + 0.7152 * g + 0.0722 * b
        sums[l] += lum
        grids[l][(y, x)] = lum
    per = len(grids[0])
    return [s / per for s in sums], grids


def cmd_stats(paths):
    for p in paths:
        means, grids = layer_lum(p)
        eye_diff = ""
        if len(grids) >= 2:
            d = sum(abs(grids[0][k] - grids[1][k]) for k in grids[0]) / len(grids[0])
            eye_diff = f" left-right mean abs diff {d:.4f}"
        print(f"{p}: mean luminance " + " ".join(f"{m:.4f}" for m in means) + eye_diff)


def cmd_diff(a, b):
    ma, ga = layer_lum(a)
    mb, gb = layer_lum(b)
    for l in range(min(len(ga), len(gb))):
        d = sum(abs(ga[l][k] - gb[l].get(k, 0)) for k in ga[l]) / len(ga[l])
        print(f"layer {l}: mean abs diff {d:.4f} (means {ma[l]:.4f} vs {mb[l]:.4f})")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    if sys.argv[1] == "png":
        cmd_png(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None)
    elif sys.argv[1] == "stats":
        cmd_stats(sys.argv[2:])
    elif sys.argv[1] == "diff":
        cmd_diff(sys.argv[2], sys.argv[3])
