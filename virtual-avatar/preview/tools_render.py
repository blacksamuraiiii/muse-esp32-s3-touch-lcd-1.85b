#!/usr/bin/env python3
"""Host-side preview harness for virtual-avatar/muse_pixel.c (Windows, no gcc needed).

Builds esp32/tools/muse/anim.c against a chosen muse_pixel.c with `zig cc`,
dumps PPM frames, then writes:
  * <out>/gifs/<state>.gif        - one GIF per state, device frame rate
  * <out>/sheet.png               - contact sheet, one key frame per state
  * <out>/zoom.png                - 3x nearest-neighbour crop of the face

    python tools_render.py [--src FILE] [--out DIR]
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
AVATAR = os.path.dirname(HERE)                      # virtual-avatar/
REPO = os.path.dirname(AVATAR)                      # repo root
ESP32 = os.path.join(REPO, "esp32")
ZIG = os.path.join(os.path.expanduser("~"), ".workbuddy-ai", "binaries", "python",
                   "envs", "default", "Lib", "site-packages", "ziglang", "zig.exe")
FRAME_MS = 40
STATES = ["boot", "idle", "listening", "thinking", "speaking", "happy", "off", "error"]


def build(src, exe, tmp):
    """Compiles anim.c + src. anim.c's POSIX mkdir is patched for MSVC-style headers."""
    patched = os.path.join(tmp, "anim_win.c")
    with open(os.path.join(ESP32, "tools", "muse", "anim.c"), "r", encoding="utf-8") as f:
        text = f.read()
    text = text.replace("#include <sys/stat.h>",
                        "#include <sys/stat.h>\n#include <direct.h>\n#define mkdir(p, m) _mkdir(p)")
    with open(patched, "w", encoding="utf-8") as f:
        f.write(text)
    subprocess.run([ZIG, "cc", "-O2", "-I", os.path.join(ESP32, "components", "muse"),
                    patched, src, "-lm", "-o", exe],
                   check=True, capture_output=True, text=True)


def frames_to_gifs(frames_dir, out_dir):
    from PIL import Image
    os.makedirs(out_dir, exist_ok=True)
    for name in sorted(os.listdir(frames_dir)):
        d = os.path.join(frames_dir, name)
        if not os.path.isdir(d):
            continue
        files = sorted(os.listdir(d))
        imgs = [Image.open(os.path.join(d, f)).convert("RGB") for f in files]
        strip = Image.new("RGB", (imgs[0].width, imgs[0].height * len(imgs)))
        for i, im in enumerate(imgs):
            strip.paste(im, (0, i * im.height))
        pal = strip.quantize(colors=256, method=Image.Quantize.MEDIANCUT)
        q = [im.quantize(palette=pal, dither=Image.Dither.NONE) for im in imgs]
        q[0].save(os.path.join(out_dir, name + ".gif"), save_all=True,
                  append_images=q[1:], duration=FRAME_MS, loop=0, optimize=False)


def contact_sheet(frames_dir, out_png, picks):
    from PIL import Image
    ims = []
    for state, idx in picks:
        path = os.path.join(frames_dir, state, "%03d.ppm" % idx)
        if not os.path.exists(path):
            continue
        ims.append((state, Image.open(path).convert("RGB")))
    if not ims:
        return
    cell = ims[0][1].width
    cols = 4
    rows = (len(ims) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * cell, rows * cell), (0, 0, 0))
    for i, (_, im) in enumerate(ims):
        sheet.paste(im, ((i % cols) * cell, (i // cols) * cell))
    sheet.save(out_png)


def zoom(frames_dir, out_png, state, idx, box, scale=3):
    from PIL import Image
    path = os.path.join(frames_dir, state, "%03d.ppm" % idx)
    if not os.path.exists(path):
        return
    im = Image.open(path).convert("RGB").crop(box)
    im = im.resize((im.width * scale, im.height * scale), Image.Resampling.NEAREST)
    im.save(out_png)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=os.path.join(AVATAR, "muse_pixel.c"))
    ap.add_argument("--out", default=os.path.join(HERE, "v5"))
    ap.add_argument("--box", default="40,60,280,300", help="zoom crop x0,y0,x1,y1 in 320px space")
    ap.add_argument("--keep", action="store_true", help="copy the raw PPM frames next to the output")
    args = ap.parse_args()

    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "muse_anim.exe")
        print("building...", flush=True)
        try:
            build(os.path.abspath(args.src), exe, tmp)
        except subprocess.CalledProcessError as e:
            sys.exit((e.stdout or "") + (e.stderr or "") + "build failed")
        frames_dir = os.path.join(tmp, "frames")
        os.makedirs(frames_dir)
        print("rendering...", flush=True)
        subprocess.check_call([exe, frames_dir])
        frames_to_gifs(frames_dir, os.path.join(out, "gifs"))
        contact_sheet(frames_dir, os.path.join(out, "sheet.png"),
                      [("idle", 40), ("idle", 90), ("listening", 30), ("thinking", 40),
                       ("speaking", 30), ("happy", 40), ("error", 40), ("off", 30)])
        box = tuple(int(v) for v in args.box.split(","))
        zoom(frames_dir, os.path.join(out, "zoom.png"), "idle", 40, box)
        zoom(frames_dir, os.path.join(out, "zoom_happy.png"), "happy", 40, box)
        if args.keep:
            keep = os.path.join(out, "frames")
            shutil.rmtree(keep, ignore_errors=True)
            shutil.copytree(frames_dir, keep)
    print("done ->", out)


if __name__ == "__main__":
    main()
