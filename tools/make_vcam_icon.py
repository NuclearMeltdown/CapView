"""Generates res/vcam_icon.bin from res/capview.ico.

The virtual camera's filter DLL draws the mark on its "CapView is not running"
picture, and that DLL is carried inside CapView.exe as a resource -- so every
byte it costs is paid twice. The 256x256 image out of the .ico is 256 KiB raw,
which is a lot to spend on a placeholder.

It is also a flat-coloured mark: 213 distinct colours in 1729 runs. A palette
and a run length encoding put it under 8 KiB, and the decoder on the other side
is twenty lines. Run it after tools/make_icon.py:

    python tools/make_vcam_icon.py

Format, little endian throughout:

    'CVIC'   magic
    uint16   width, height
    uint16   paletteCount, runCount
    BGRA     palette[paletteCount]
    uint16   count, index          runCount times

Straight (non-premultiplied) alpha, top-down rows, exactly as the .ico stores
its colours -- the premultiply happens in the decoder, where the background is
known.
"""

import os
import struct
import sys

MAGIC = b"CVIC"
SIZE = 256  # which image out of the .ico to take
MAX_RUN = 0xFFFF

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SOURCE = os.path.join(ROOT, "res", "capview.ico")
TARGET = os.path.join(ROOT, "res", "vcam_icon.bin")


def read_ico_image(path, wanted):
    """Returns (width, height, pixels) with pixels as top-down BGRA bytes."""
    with open(path, "rb") as handle:
        data = handle.read()

    reserved, kind, count = struct.unpack("<HHH", data[:6])
    if reserved != 0 or kind != 1:
        raise SystemExit("%s is not an icon file" % path)

    for i in range(count):
        entry = data[6 + i * 16:22 + i * 16]
        w, h, _colors, _rsv, _planes, _bits, size, offset = struct.unpack("<BBBBHHII", entry)
        w = w or 256
        h = h or 256
        if w != wanted or h != wanted:
            continue

        header = struct.unpack("<IiiHHIIiiII", data[offset:offset + 40])
        if header[4] != 32 or header[5] != 0:
            raise SystemExit("the %dx%d image is not uncompressed 32-bit" % (w, h))

        # The XOR mask only. An icon's BITMAPINFOHEADER claims twice the height
        # because the AND mask follows, and a 32-bit image carries its own
        # alpha, so that mask is of no interest here.
        start = offset + 40
        rows = []
        for y in range(h - 1, -1, -1):  # stored bottom-up
            begin = start + y * w * 4
            rows.append(data[begin:begin + w * 4])
        return w, h, b"".join(rows)

    raise SystemExit("no %dx%d image in %s" % (wanted, wanted, path))


def pack(width, height, pixels):
    palette = []
    index_of = {}
    indices = []
    for i in range(width * height):
        colour = pixels[i * 4:i * 4 + 4]
        # Every fully transparent pixel is the same pixel, whatever colour the
        # editor left underneath it. Folding them together costs nothing and
        # keeps the runs long.
        if colour[3] == 0:
            colour = b"\x00\x00\x00\x00"
        found = index_of.get(colour)
        if found is None:
            found = len(palette)
            index_of[colour] = found
            palette.append(colour)
        indices.append(found)

    if len(palette) > 0xFFFF:
        raise SystemExit("palette of %d does not fit" % len(palette))

    runs = []
    run_index = indices[0]
    run_length = 1
    for value in indices[1:]:
        if value == run_index and run_length < MAX_RUN:
            run_length += 1
        else:
            runs.append((run_length, run_index))
            run_index = value
            run_length = 1
    runs.append((run_length, run_index))

    if len(runs) > 0xFFFF:
        raise SystemExit("%d runs do not fit" % len(runs))

    out = bytearray(MAGIC)
    out += struct.pack("<HHHH", width, height, len(palette), len(runs))
    for colour in palette:
        out += colour
    for length, value in runs:
        out += struct.pack("<HH", length, value)
    return bytes(out), palette, runs


def unpack(blob):
    """Decodes again, so the check below is against the real thing."""
    if blob[:4] != MAGIC:
        raise SystemExit("bad magic")
    width, height, colours, runs = struct.unpack("<HHHH", blob[4:12])
    at = 12
    palette = [blob[at + i * 4:at + i * 4 + 4] for i in range(colours)]
    at += colours * 4
    out = bytearray()
    for _ in range(runs):
        length, value = struct.unpack("<HH", blob[at:at + 4])
        at += 4
        out += palette[value] * length
    if len(out) != width * height * 4:
        raise SystemExit("decoded %d bytes, wanted %d" % (len(out), width * height * 4))
    return width, height, bytes(out)


def main():
    width, height, pixels = read_ico_image(SOURCE, SIZE)
    blob, palette, runs = pack(width, height, pixels)

    # Round trip before writing. A silently wrong icon would only show up in
    # the one picture nobody looks at until something has already gone wrong.
    back_w, back_h, back = unpack(blob)
    if (back_w, back_h) != (width, height):
        raise SystemExit("round trip changed the size")
    for i in range(width * height):
        want = pixels[i * 4:i * 4 + 4]
        if want[3] == 0:
            want = b"\x00\x00\x00\x00"
        if back[i * 4:i * 4 + 4] != want:
            raise SystemExit("round trip differs at pixel %d" % i)

    with open(TARGET, "wb") as handle:
        handle.write(blob)

    raw = width * height * 4
    print("%s: %dx%d, %d colours, %d runs, %d bytes (%.1f%% of %d raw)"
          % (os.path.relpath(TARGET, ROOT), width, height, len(palette), len(runs),
             len(blob), 100.0 * len(blob) / raw, raw))
    return 0


if __name__ == "__main__":
    sys.exit(main())
