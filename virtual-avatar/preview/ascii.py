#!/usr/bin/env python3
"""Dumps one 64x64 frame from a rendered PPM as an ASCII map, for pixel-level checking."""
import sys
from PIL import Image

PAL = {
    (0, 0, 0): '.',          # bg
    (0x2a, 0x21, 0x34): '#', # out
    (0x0e, 0x0c, 0x13): '@', # hair
    (0x28, 0x22, 0x33): '%', # hair sheen
    (0x35, 0x2c, 0x48): 'r', # hair rim
    (0xff, 0xc9, 0xa0): ' ', # skin
    (0xe9, 0xa8, 0x7c): 's', # skin shade
    (0xff, 0xff, 0xff): 'W', # eye white / shine
    (0x62, 0xc2, 0x57): 'G', # iris
    (0x3f, 0x8f, 0x41): 'g', # iris dark
    (0x0a, 0x0a, 0x0e): 'B', # pupil
    (0x7d, 0x5f, 0x96): 'b', # brow
    (0x54, 0x24, 0x2c): 'm', # mouth
    (0xe8, 0x70, 0x7c): 't', # tongue
    (0x4f, 0xbf, 0x5a): 'D', # dress
    (0x10, 0x10, 0x14): 'K', # stripe
    (0xf2, 0x9a, 0x9e): 'p', # blush
}


def q565(rgb):
    """Same round trip the renderer does: 8 -> 5/6/5 -> 8."""
    r, g, b = rgb
    return ((r >> 3) * 255 // 31, (g >> 2) * 255 // 63, (b >> 3) * 255 // 31)


PAL = {q565(k): ch for k, ch in PAL.items()}


def near(rgb):
    return PAL.get(tuple(rgb), '?')


def main():
    path = sys.argv[1]
    y0 = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    y1 = int(sys.argv[3]) if len(sys.argv) > 3 else 63
    im = Image.open(path).convert("RGB")
    s = im.width / 64
    print("   " + "".join(str(x // 10 or " ") for x in range(64)))
    print("   " + "".join(str(x % 10) for x in range(64)))
    for cy in range(y0, y1 + 1):
        row = "".join(near(im.getpixel((int((cx + 0.5) * s), int((cy + 0.5) * s)))) for cx in range(64))
        print("%2d " % cy + row)


if __name__ == "__main__":
    main()
