# SPDX-License-Identifier: GPL-3.0-or-later
"""Draws the project's app icon: a gold obelisk inside dashed rings, on a deep teal tile (the launcher's colours). No outside artwork
and no font is used: every shape is drawn here. Needs Pillow.

    python launcher/icon/make_icon.py [--out-png PATH] [--no-ico] [--no-sizes]

Writes launcher/icon/icon.png (1024 px), the sizes the launcher is built with (launcher/public/icon.png and
launcher/src-tauri/icons/*, the same set `npx tauri icon` makes for this project) and game-build/fefw/res/fefw.ico (16 to 256 px).
"""
import argparse
import math
import os

from PIL import Image, ImageChops, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
S = 2048
ICO_SIZES = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]

GOLD_LIGHT = (255, 236, 170)
GOLD = (233, 184, 58)
GOLD_DARK = (150, 98, 22)
GOLD_SHADE = (104, 66, 14)


def radial(size, inner, outer, center=(0.5, 0.42), radius=0.8):
    """A radial gradient from inner (at center) to outer."""
    small = 256
    g = Image.new('RGB', (small, small))
    px = g.load()
    cx, cy = center[0] * small, center[1] * small
    for y in range(small):
        for x in range(small):
            t = min(1.0, math.hypot(x - cx, y - cy) / (radius * small))
            t = t * t * (3 - 2 * t)
            px[x, y] = tuple(int(inner[i] + (outer[i] - inner[i]) * t) for i in range(3))
    return g.resize((size, size), Image.BICUBIC)


def vertical(size, top, bottom):
    g = Image.new('RGB', (1, 256))
    for y in range(256):
        t = y / 255
        g.putpixel((0, y), tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)))
    return g.resize((size, size), Image.BICUBIC)


def dashed_ring(draw, cx, cy, r, width, dash_deg, gap_deg, fill, start=0.0):
    a = start
    while a < start + 360:
        draw.arc((cx - r, cy - r, cx + r, cy + r), a, min(a + dash_deg, start + 360), fill=fill, width=width)
        a += dash_deg + gap_deg


def pts(points):
    return [(x * S, y * S) for x, y in points]


def draw_icon():
    radius = int(S * 0.225)
    tile_mask = Image.new('L', (S, S), 0)
    ImageDraw.Draw(tile_mask).rounded_rectangle((0, 0, S - 1, S - 1), radius=radius, fill=255)
    img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    img.paste(radial(S, (34, 86, 94), (6, 20, 24)), (0, 0), tile_mask)

    # a fine pale frame inside the tile, as the launcher's panels have
    frame = Image.new('L', (S, S), 0)
    inset = int(S * 0.055)
    ImageDraw.Draw(frame).rounded_rectangle((inset, inset, S - 1 - inset, S - 1 - inset), radius=radius - inset, outline=255, width=max(2, S // 170))
    img.paste(Image.new('RGBA', (S, S), (190, 196, 160, 255)), (0, 0), frame.point(lambda v: v * 130 // 255))

    # the dashed rings of the title screen, behind the obelisk
    rings = Image.new('L', (S, S), 0)
    rd = ImageDraw.Draw(rings)
    cx, cy = S * 0.5, S * 0.52
    dashed_ring(rd, cx, cy, S * 0.345, max(3, S // 230), 22, 7, 255, start=8)
    dashed_ring(rd, cx, cy, S * 0.285, max(2, S // 330), 5, 6, 200, start=0)
    img.paste(Image.new('RGBA', (S, S), (224, 184, 76, 255)), (0, 0), rings.point(lambda v: v * 150 // 255))

    # the obelisk: a tapered shaft with a pointed cap on a low base, the light from the upper left
    top, shoulder, foot = 0.17, 0.30, 0.74
    half_top, half_foot = 0.085, 0.118
    left = pts([(0.5, top), (0.5 - half_top, shoulder), (0.5 - half_foot, foot), (0.5, foot)])
    right = pts([(0.5, top), (0.5 + half_top, shoulder), (0.5 + half_foot, foot), (0.5, foot)])
    base = pts([(0.5 - 0.17, foot), (0.5 + 0.17, foot), (0.5 + 0.20, foot + 0.07), (0.5 - 0.20, foot + 0.07)])
    whole = Image.new('L', (S, S), 0)
    wd = ImageDraw.Draw(whole)
    for poly in (left, right, base):
        wd.polygon(poly, fill=255)

    # a soft glow and a drop shadow so it lifts off the tile
    glow = Image.new('RGBA', (S, S), (240, 190, 70, 0))
    glow.putalpha(whole.filter(ImageFilter.GaussianBlur(S * 0.05)).point(lambda v: v * 90 // 255))
    img = Image.alpha_composite(img, glow)
    shadow = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    shadow.putalpha(whole.filter(ImageFilter.GaussianBlur(S * 0.012)).point(lambda v: v * 190 // 255))
    img = Image.alpha_composite(img, ImageChops.offset(shadow, int(S * 0.006), int(S * 0.014)))

    def fill(poly, top_colour, bottom_colour):
        mask = Image.new('L', (S, S), 0)
        ImageDraw.Draw(mask).polygon(poly, fill=255)
        layer = vertical(S, top_colour, bottom_colour).convert('RGBA')
        layer.putalpha(mask)
        return layer

    img = Image.alpha_composite(img, fill(left, GOLD_LIGHT, GOLD))
    img = Image.alpha_composite(img, fill(right, GOLD, GOLD_DARK))
    img = Image.alpha_composite(img, fill(base, GOLD, GOLD_SHADE))

    # the edges that give it its facets: the centre line, the shoulder, the base's top
    lines = Image.new('L', (S, S), 0)
    ld = ImageDraw.Draw(lines)
    w = max(3, S // 260)
    ld.line(pts([(0.5, top + 0.01), (0.5, foot)]), fill=255, width=w)
    ld.line(pts([(0.5 - half_top, shoulder), (0.5, shoulder + 0.035), (0.5 + half_top, shoulder)]), fill=255, width=w)
    ld.line(pts([(0.5 - 0.17, foot), (0.5 + 0.17, foot)]), fill=255, width=w)
    img.paste(Image.new('RGBA', (S, S), (255, 246, 208, 255)), (0, 0), ImageChops.multiply(lines, whole).point(lambda v: v * 150 // 255))

    # a mark on the shaft, as the game's obelisks carry one: a small diamond
    d = 0.026
    mark = Image.new('L', (S, S), 0)
    ImageDraw.Draw(mark).polygon(pts([(0.5, 0.50 - d * 1.6), (0.5 + d, 0.50), (0.5, 0.50 + d * 1.6), (0.5 - d, 0.50)]), fill=255)
    img.paste(Image.new('RGBA', (S, S), (58, 36, 6, 255)), (0, 0), mark.point(lambda v: v * 210 // 255))

    out = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    out.paste(img, (0, 0), tile_mask)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out-png', default=os.path.join(HERE, 'icon.png'))
    ap.add_argument('--no-ico', action='store_true', help='leave game-build/fefw/res/fefw.ico alone')
    ap.add_argument('--no-sizes', action='store_true', help='leave launcher/public and launcher/src-tauri/icons alone')
    a = ap.parse_args()
    big = draw_icon()
    size = lambda n: big.resize((n, n), Image.LANCZOS)
    size(1024).save(a.out_png)
    if not a.no_sizes:
        launcher = os.path.dirname(HERE)
        icons = os.path.join(launcher, 'src-tauri', 'icons')
        size(1024).save(os.path.join(launcher, 'public', 'icon.png'))
        size(32).save(os.path.join(icons, '32x32.png'))
        size(128).save(os.path.join(icons, '128x128.png'))
        size(256).save(os.path.join(icons, '128x128@2x.png'))
        size(512).save(os.path.join(icons, 'icon.png'))
        size(256).save(os.path.join(icons, 'icon.ico'), sizes=[s for s in ICO_SIZES if s != (128, 128)])
    if not a.no_ico:
        ico = os.path.join(ROOT, 'game-build', 'fefw', 'res', 'fefw.ico')
        os.makedirs(os.path.dirname(ico), exist_ok=True)
        size(256).save(ico, sizes=ICO_SIZES)
    print('wrote', a.out_png)


if __name__ == '__main__':
    main()
