"""Draws the Tuki tray/app icons (a mouse silhouette) and writes PNG-compressed .ico files.

Pure standard library: run `python tools/make_icon.py` from the repo root.
"""
import math
import struct
import zlib
from pathlib import Path

SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
SUPERSAMPLE = 4

BEAK = (0xF2, 0x9F, 0x05)      # active body
BEAK_EDGE = (0x8A, 0x52, 0x00)
SLATE = (0x8E, 0x94, 0x9C)     # paused body
SLATE_EDGE = (0x4A, 0x4F, 0x57)
INK = (0x17, 0x19, 0x1C)


def rounded_rect_sdf(px, py, cx, cy, hx, hy, r):
    qx = abs(px - cx) - (hx - r)
    qy = abs(py - cy) - (hy - r)
    outside = math.hypot(max(qx, 0.0), max(qy, 0.0))
    inside = min(max(qx, qy), 0.0)
    return outside + inside - r


def shade(u, v, body, edge):
    """Colour at normalized point (u, v) in [0,1]^2, or None for transparent."""
    # Mouse body: tall capsule.
    d = rounded_rect_sdf(u, v, 0.5, 0.5, 0.30, 0.44, 0.30)
    if d > 0:
        return None
    outline = 0.045
    color = edge if d > -outline else body
    # Seam between the two buttons, from the top down to the wheel.
    if abs(u - 0.5) < 0.028 and 0.06 < v < 0.42:
        color = INK
    # Scroll wheel.
    if rounded_rect_sdf(u, v, 0.5, 0.30, 0.065, 0.11, 0.065) < 0:
        color = INK
    return color


def render(size, body, edge):
    rows = []
    n = SUPERSAMPLE
    for y in range(size):
        row = bytearray()
        for x in range(size):
            r = g = b = a = 0.0
            for sy in range(n):
                for sx in range(n):
                    u = (x + (sx + 0.5) / n) / size
                    v = (y + (sy + 0.5) / n) / size
                    c = shade(u, v, body, edge)
                    if c is not None:
                        r += c[0]
                        g += c[1]
                        b += c[2]
                        a += 1
            samples = n * n
            if a:
                row += bytes((round(r / a), round(g / a), round(b / a), round(255 * a / samples)))
            else:
                row += b"\0\0\0\0"
        rows.append(bytes(row))
    return rows


def png_bytes(size, rows):
    raw = b"".join(b"\0" + row for row in rows)

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def write_ico(path, body, edge):
    images = [png_bytes(s, render(s, body, edge)) for s in SIZES]
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries = b""
    for size, data in zip(SIZES, images):
        dim = 0 if size >= 256 else size
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    path.write_bytes(header + entries + b"".join(images))
    return images


if __name__ == "__main__":
    root = Path(__file__).resolve().parent.parent
    assets = root / "assets"
    assets.mkdir(exist_ok=True)
    images = write_ico(assets / "tuki.ico", BEAK, BEAK_EDGE)
    write_ico(assets / "tuki-paused.ico", SLATE, SLATE_EDGE)
    (assets / "tuki-256.png").write_bytes(images[-1])
    (root / "ui" / "favicon.png").write_bytes(png_bytes(64, render(64, BEAK, BEAK_EDGE)))
    print("icons written to", assets)
