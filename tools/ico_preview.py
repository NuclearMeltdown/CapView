"""Renders a PNG contact sheet of the icon sizes, for eyeballing the result."""
import os, struct, sys, zlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_icon

def write_png(path, width, height, rows_rgba):
    raw = b"".join(b"\x00" + bytes(row) for row in rows_rgba)
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)

def clamp8(v):
    return int(round(min(max(v, 0.0), 255.0)))

sizes = [256, 64, 48, 32, 24, 16]
pad = 12
bg = (46, 46, 52, 255)
width = pad + sum(s + pad for s in sizes)
height = 256 + pad * 2

rows = [[] for _ in range(height)]
for y in range(height):
    row = []
    for x in range(width):
        row.extend(bg)
    rows[y] = row

x_cursor = pad
for s in sizes:
    px = make_icon.render(s)
    y_off = pad + (256 - s) // 2
    for iy in range(s):
        for ix in range(s):
            r, g, b, a = px[iy * s + ix]
            X = x_cursor + ix
            Y = y_off + iy
            base = X * 4
            dr, dg, db = bg[0], bg[1], bg[2]
            rows[Y][base + 0] = clamp8(r * a + dr * (1 - a))
            rows[Y][base + 1] = clamp8(g * a + dg * (1 - a))
            rows[Y][base + 2] = clamp8(b * a + db * (1 - a))
            rows[Y][base + 3] = 255
    x_cursor += s + pad

out = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "res", "icon_preview.png")
write_png(out, width, height, rows)
print("geschrieben:", out)
