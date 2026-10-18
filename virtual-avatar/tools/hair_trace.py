#!/usr/bin/env python3
"""Trace the reference photo's hair into the 64x64 dot matrix used by v8.

    python3 tools/hair_trace.py --dump          # 64x64 ASCII of the matrix
    python3 tools/hair_trace.py --table         # the C initialiser (paste into muse_pixel.c)
    python3 tools/hair_trace.py --png out.png   # the matrix painted over a v8 frame

Why this exists: the reference (maomao/毛毛头像.png) draws the hair as flat
black shapes on a transparent field, so the hair outline is already a dot
matrix - it only has to be sampled, not interpreted. v8's HAIR_BITS table is
this script's output; run it again after re-cropping or re-mapping the photo.

The three things that make the raw downsample usable:

  * the bold black linework that is NOT hair (eye outlines, pupils, mouth, jaw)
    is dropped before sampling - by ellipse masks where the shape is known and
    by a Y_CUT where the jaw outline would otherwise read as a lock of hair;
  * an opening (erode+dilate, OPEN px) erases whatever bold line survived, so
    only masses wide enough to survive 64px sampling are kept;
  * the photo is drawn very slightly rotated - the two side locks differ by up
    to 3 cells - so the left half is mirrored onto the right.

Mapping, chosen to keep v7's head box (dome top ~row 5, fringe cut on row 16,
the two flip tips on columns 5 / 59):
    row = 16 + (y - 300) * 0.0523
    col = 32 + (x - 897.5) * 0.0442
Change FY/FS/FX/FSC to re-fit the hair to a different face.
"""
import argparse
import os

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
AVATAR = os.path.dirname(HERE)
DEFAULT_SRC = os.path.join(AVATAR, "maomao", "毛毛头像.png")

# photo -> grid
FY, FS = 300.0, 0.0523      # y of grid row 16, cells per px (vertical)
FX, FSC = 897.5, 0.0442     # x of grid column 32, cells per px (horizontal)
CH, CW = 1 / FS, 1 / FSC

Y_CUT = 765                 # below this row the black is the jaw outline
OPEN = 15                   # opening kernel, px: erases linework thinner than this
THRESH = 0.45               # hair if this much of a cell's footprint is hair
SMOOTH = (18, 32, 25, 40)   # rows/cols of the nose bridge: skin, never hair


def load(path):
    a = np.array(Image.open(path)).astype(int)
    rgb = a[..., :3]
    R, G, B = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    lum = (R * 299 + G * 587 + B * 114) / 1000
    return dict(fg=a[..., 3] > 128, R=R, G=G, B=B, lum=lum)


def raw_mask(d):
    """Black pixels that are hair: eyes, mouth and the jaw outline removed."""
    fg, lum = d['fg'], d['lum']
    dark = fg & (lum < 70)
    yy, xx = np.mgrid[0:fg.shape[0], 0:fg.shape[1]]
    feat = np.zeros_like(fg)
    # eye ellipses incl. the bold outline: (centre, rx, ry) measured off the photo
    for (ex, ey), rx, ry in (((606.5, 540.5), 205, 220), ((1188.0, 534.0), 199, 214)):
        feat |= ((xx - ex) ** 2 / (rx + 15) ** 2 + (yy - ey) ** 2 / (ry + 15) ** 2) <= 1.0
    feat[640:900, 740:1030] = True      # mouth
    feat[Y_CUT:, :] = True              # jaw outline
    return (dark & ~feat).astype(np.uint8)


def box_mean(a, k):
    pad = k // 2
    p = np.pad(a.astype(np.float32), pad, mode='constant')
    c = np.pad(p.cumsum(0).cumsum(1), ((1, 0), (1, 0)))
    return (c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]) / (k * k)


def opening(a, k):
    er = (box_mean(a, k) > 0.999).astype(np.uint8)
    return (box_mean(er, k) > 0.001).astype(np.uint8)


def trace(path=DEFAULT_SRC):
    m = opening(raw_mask(load(path)), OPEN)
    g = np.zeros((64, 64), bool)
    for r in range(64):
        y0 = FY + (r - 16 - 0.5) * CH
        ya, yb = max(0, int(y0)), max(1, int(y0 + CH))
        for c in range(64):
            x0 = FX + (c - 32 - 0.5) * CW
            xa, xb = max(0, int(x0)), max(1, int(x0 + CW))
            sub = m[ya:yb, xa:xb]
            g[r, c] = sub.mean() > THRESH if sub.size else False
    r0, r1, c0, c1 = SMOOTH
    g[r0:r1, c0:c1] = False
    for r in range(64):
        for c in range(33, 64):
            g[r, c] = g[r][64 - c]
    return g


def dump(g):
    print('     ' + ''.join(str(c // 10 % 10) for c in range(64)))
    print('     ' + ''.join(str(c % 10) for c in range(64)))
    for r in range(64):
        print('%2d   %s' % (r, ''.join('#' if v else '.' for v in g[r])))


def table(g, per_line=4):
    words = []
    for r in range(64):
        v = 0
        for c in range(64):
            if g[r, c]:
                v |= 1 << (63 - c)
        words.append('0x%016xULL,' % v)
    return '\n'.join('    ' + ' '.join(words[i:i + per_line])
                     for i in range(0, 64, per_line))


def png(g, out, frame=None):
    """Paint the matrix over a rendered frame so the mapping can be eyeballed."""
    if frame is None:
        a = np.zeros((320, 320, 3), np.uint8)
    else:
        a = np.array(Image.open(frame).convert('RGB').resize((320, 320), Image.Resampling.NEAREST))
    for r in range(64):
        for c in range(64):
            if g[r, c]:
                a[r * 5:r * 5 + 5, c * 5:c * 5 + 5] = (255, 0, 255)
    Image.fromarray(a).save(out)


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', default=DEFAULT_SRC)
    ap.add_argument('--dump', action='store_true')
    ap.add_argument('--table', action='store_true')
    ap.add_argument('--png')
    ap.add_argument('--frame', help='rendered frame to paint the matrix over')
    a = ap.parse_args()
    m = trace(a.src)
    if a.table:
        print(table(m))
    elif a.png:
        png(m, a.png, a.frame)
    else:
        dump(m)
