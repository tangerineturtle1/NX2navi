"""Draws the pictures of the handover window into bridge/launcher/art (run once, by whoever changes the look).

    python tools/make_art.py            needs Pillow and numpy; reads tools/art-src/triforce.png and nx2navi_logo.png

The window (bridge/launcher/handover.py) is one fixed composition of 820 x 480 units. Everything that needs soft light
(the hall, the halo behind the crest, the crest, the wordmark, the fairy) is drawn here, once per display scale, so the
player's PC only puts text and buttons on top of a PNG: no picture library is needed there.

    hall_<scale>.png      the whole background, <scale> = 100, 125, 150, 200 (percent)
    btn_<scale>.png       the gold bar of the primary button, and btn_<scale>_hi.png for the mouse over it
    icon_64/32/16.png     the crest for the title bar and the taskbar

The look is the Triforce start client's ("the relic in a dark hall"): a warm near-black page, one light from above, a
gold halo and a dial's arc behind the painted crest, the wordmark under it. The fairy is NX2navi's logo, kept pixel
exact (whole cells, hard edges), arriving beside the crest.
"""
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'tools' / 'art-src'
OUT = ROOT / 'bridge' / 'launcher' / 'art'

W, H = 820, 480                        # the window, in units (pixels at 100 %)
CX, CREST_W, CREST_TOP = 632, 228, 70  # the crest's centre line, width and top: handover.py leaves this column free
SCALES = (1.0, 1.25, 1.5, 2.0)
SS = 2                                 # everything soft is drawn at twice the size and scaled down

INK = (0x0A, 0x09, 0x08)
GOLD = (0xE0, 0xB2, 0x3E)
GOLD_100, GOLD_300, GOLD_700 = (0xFF, 0xF4, 0xCC), (0xF7, 0xDC, 0x8C), (0x9A, 0x6A, 0x18)


def grid(h, w):
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    return xx + 0.5, yy + 0.5


def ramp(t, stops):
    return np.interp(t, [p for p, _ in stops], [a for _, a in stops]).astype(np.float32)


def over(base, color, alpha):
    c = np.array(color, np.float32) / 255
    return base * (1 - alpha[..., None]) + c * alpha[..., None]


def mask_of(size, draw):
    """An 8-bit mask drawn with PIL, as a float array 0..1."""
    m = Image.new('L', size, 0)
    draw(ImageDraw.Draw(m))
    return np.asarray(m, np.float32) / 255


def crest_art(width):
    im = Image.open(SRC / 'triforce.png').convert('RGBA')
    return im.resize((width, round(width * im.height / im.width)), Image.LANCZOS)


def navi_cells():
    """NX2navi's logo as its 22 x 21 cells (the source picture is each cell 54 x 54)."""
    im = Image.open(SRC / 'nx2navi_logo.png').convert('RGBA')
    cw, ch = im.width // 22, im.height // 21
    cells = Image.new('RGBA', (22, 21))
    for y in range(21):
        for x in range(22):
            cells.putpixel((x, y), im.getpixel((x * cw + cw // 2, y * ch + ch // 2)))
    return cells


def wordmark(k):
    """TRIFORCE in engraved gold with its shadow, as an RGBA picture at k pixels per unit."""
    font = ImageFont.truetype('palab.ttf', round(21 * k))
    gap = 0.40 * 21 * k
    letters = 'TRIFORCE'
    widths = [font.getlength(c) for c in letters]
    total = sum(widths) + gap * (len(letters) - 1)
    pad = round(12 * k)
    size = (round(total) + 2 * pad, round(34 * k) + 2 * pad)
    text = Image.new('L', size, 0)
    d = ImageDraw.Draw(text)
    x = pad
    for c, w in zip(letters, widths):
        d.text((x, pad), c, font=font, fill=255)
        x += w + gap
    a = np.asarray(text, np.float32) / 255
    box = text.getbbox()
    t = np.clip((np.arange(size[1], dtype=np.float32) - box[1]) / max(1, box[3] - box[1]), 0, 1)
    stops = [(0, (0xFF, 0xF1, 0xB8)), (0.42, (0xE8, 0xB8, 0x40)), (0.56, (0xB5, 0x7E, 0x1C)), (1, (0xF3, 0xCF, 0x6A))]
    rgb = np.stack([np.interp(t, [p for p, _ in stops], [c[i] for _, c in stops]) for i in range(3)], -1)
    rgb = np.repeat(rgb[:, None, :], size[0], 1).astype(np.float32)
    shadow = np.asarray(Image.fromarray((a * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(3 * k)),
                        np.float32) / 255
    shadow = np.roll(shadow, round(2 * k), 0) * 0.8
    alpha = a + shadow * (1 - a)
    color = np.where(alpha[..., None] > 0, rgb * (a / np.maximum(alpha, 1e-6))[..., None], 0)
    out = np.dstack([color, alpha * 255]).clip(0, 255).astype(np.uint8)
    return Image.fromarray(out, 'RGBA'), total, pad


def hall(scale):
    k = scale * SS
    w, h = round(W * k), round(H * k)
    xx, yy = grid(h, w)
    img = np.empty((h, w, 3), np.float32)
    img[:] = np.array(INK, np.float32) / 255

    crest = crest_art(round(CREST_W * k))
    cw, ch = crest.size
    cl, ct = round(CX * k - cw / 2), round(CREST_TOP * k)
    ccx, ccy = CX * k, ct + ch / 2

    # 2. the light from above
    img = over(img, (0xFF, 0xEC, 0xAA), ramp(yy / h, [(0, 0.045), (0.45, 0)]))
    # 3. the halo behind the crest
    halo_r = 1.2 * cw
    t = np.sqrt((xx - ccx) ** 2 + (yy - ccy) ** 2) / halo_r
    img = over(img, GOLD, ramp(t, [(0, 0.24), (0.45, 0.075), (1, 0)]))
    # 4. the lattice of triangles, only inside the halo
    side, row = 56 * k, 56 * k * math.sqrt(3) / 2

    def lattice(d):
        lw = max(1, round(k * 0.9))
        y = ccy % row - row
        while y < h:
            d.line([(0, y), (w, y)], fill=255, width=lw)
            y += row
        slope = math.tan(math.radians(60))
        x0 = ccx - (ccy / slope)
        n = int(w / side) + 6
        for i in range(-n, n):
            xa = x0 + i * side
            d.line([(xa, 0), (xa + h / slope, h)], fill=255, width=lw)
            xb = x0 + 2 * (ccy / slope) + i * side
            d.line([(xb, 0), (xb - h / slope, h)], fill=255, width=lw)
    img = over(img, GOLD, mask_of((w, h), lattice) * 0.06 * ramp(t, [(0, 1), (0.5, 0.5), (0.95, 0)]))
    # 6. the vignette (before the arc, which sits in front of it)
    tv = np.sqrt(((xx - w / 2) / (0.78 * w)) ** 2 + ((yy - h / 2) / (0.78 * h)) ** 2)
    img = over(img, (0, 0, 0), ramp(tv, [(0, 0), (0.55, 0), (1, 0.55)]))
    # 5. the arc over the crest: a ring and a ring of marks, fading out on the way down
    acx, acy = ccx, ccy + 0.06 * ch
    r_in, r_out = 0.66 * cw, 0.705 * cw

    def ring(d):
        d.ellipse([acx - r_in, acy - r_in, acx + r_in, acy + r_in], outline=255, width=max(1, round(k * 0.9)))

    def marks(d):
        for i in range(120):
            a = 2 * math.pi * i / 120
            ca, sa = math.cos(a), math.sin(a)
            d.line([(acx + (r_out - 1.5 * k) * ca, acy + (r_out - 1.5 * k) * sa),
                    (acx + (r_out + 1.5 * k) * ca, acy + (r_out + 1.5 * k) * sa)], fill=255, width=max(1, round(k * 0.9)))
    box_top, box_h = acy - r_out, 2 * r_out
    fade = ramp((yy - box_top) / box_h, [(0, 1), (0.42, 1), (0.74, 0), (1, 0)])
    img = over(img, GOLD, mask_of((w, h), ring) * 0.17 * fade)
    img = over(img, GOLD, mask_of((w, h), marks) * 0.28 * fade)
    # the floor glow under the crest
    fx, fy = ccx, ct + ch - 2 * k
    tf = np.sqrt(((xx - fx) / (0.43 * cw)) ** 2 + ((yy - fy) / (15 * k)) ** 2)
    img = over(img, GOLD, ramp(tf, [(0, 0.26), (0.55, 0.08), (1, 0)]))
    # the fairy's light, where she will be put (soft, so it is drawn here; her pixels go on after the scaling down)
    n = max(2, round(2 * scale))                       # pixels per cell of the logo, in the finished picture
    nx, ny = round((CX - CREST_W / 2 - 34) * scale), round((CREST_TOP + 26) * scale)
    ncx, ncy = (nx + 11 * n) * SS, (ny + 10.5 * n) * SS
    tn = np.sqrt((xx - ncx) ** 2 + (yy - ncy) ** 2) / (46 * k)
    img = over(img, (0x8F, 0xC0, 0xFF), ramp(tn, [(0, 0.30), (0.4, 0.10), (1, 0)]))

    pic = Image.fromarray((img.clip(0, 1) * 255).astype(np.uint8), 'RGB').convert('RGBA')
    pic.alpha_composite(crest, (cl, ct))
    wm, total, pad = wordmark(k)
    wm_top = ct + ch + round(22 * k)
    pic.alpha_composite(wm, (round(ccx - total / 2) - pad, wm_top - pad))
    # the rule with its diamond, under the wordmark
    rule_y, half = wm_top + round(46 * k), 90 * k
    rule = np.asarray(pic, np.float32) / 255
    rgb = rule[..., :3]
    line = ((np.abs(yy - rule_y) < max(0.5, k * 0.5)) & (np.abs(xx - ccx) < half)).astype(np.float32)
    rgb = over(rgb, GOLD, line * 0.55 * ramp(np.abs(xx - ccx) / half, [(0, 1), (0.7, 1), (1, 0)]))
    diamond = ((np.abs(xx - ccx) + np.abs(yy - rule_y)) < 3.2 * k).astype(np.float32)
    rgb = over(rgb, GOLD_300, diamond)
    # down to the display's size, in float, then dithered: a dark gradient bands in 8 bits without it
    small = np.asarray(Image.fromarray((rgb.clip(0, 1) * 255).astype(np.uint8), 'RGB')
                       .resize((round(W * scale), round(H * scale)), Image.LANCZOS), np.float32)
    rng = np.random.default_rng(7)
    small = small + (rng.random(small.shape, np.float32) - rng.random(small.shape, np.float32)) * 0.6
    out = Image.fromarray(small.clip(0, 255).astype(np.uint8), 'RGB').convert('RGBA')

    # the fairy: whole cells, hard edges, and a short trail of sparks she came along
    cells = navi_cells().resize((22 * n, 21 * n), Image.NEAREST)
    out.alpha_composite(cells, (nx, ny))
    d = ImageDraw.Draw(out, 'RGBA')
    # (the trail stays right of x = 420: the text column of handover.py ends at 400)
    trail = [(-14, 28, (0x8F, 0xC0, 0xFF), 160), (-26, 40, (0xFF, 0xFF, 0xFF), 125), (-40, 46, (0x8F, 0xC0, 0xFF), 95),
             (-52, 60, (0xC8, 0xE0, 0xFF), 65), (-64, 66, (0x8F, 0xC0, 0xFF), 38)]
    for dx, dy, c, a in trail:
        px, py = nx + round(dx * scale / n) * n, ny + round(dy * scale / n) * n
        d.rectangle([px, py, px + n - 1, py + n - 1], fill=c + (a,))
    return out.convert('RGB')


def button(scale, hot):
    k = scale * SS
    w, h = round(196 * k), round(44 * k)
    t = (np.arange(h, dtype=np.float32) + 0.5) / h
    stops = [(0, GOLD_100), (0.12, GOLD_300), (0.48, GOLD), (1, GOLD_700)]
    rgb = np.stack([np.interp(t, [p for p, _ in stops], [c[i] for _, c in stops]) for i in range(3)], -1)
    rgb = np.repeat(rgb[:, None, :], w, 1).astype(np.float32)
    if hot:
        rgb = rgb * 0.82 + 255 * 0.18
    rgb[:max(1, round(k * 0.8))] = rgb[:max(1, round(k * 0.8))] * 0.5 + np.array(GOLD_100, np.float32) * 0.5
    a = Image.new('L', (w, h), 0)
    ImageDraw.Draw(a).rounded_rectangle([0, 0, w - 1, h - 1], radius=round(2 * k), fill=255)
    pic = Image.fromarray(rgb.clip(0, 255).astype(np.uint8), 'RGB').convert('RGBA')
    pic.putalpha(a)
    return pic.resize((round(196 * scale), round(44 * scale)), Image.LANCZOS)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for s in SCALES:
        tag = f'{round(s * 100)}'
        hall(s).save(OUT / f'hall_{tag}.png', optimize=True)
        button(s, False).save(OUT / f'btn_{tag}.png', optimize=True)
        button(s, True).save(OUT / f'btn_{tag}_hi.png', optimize=True)
        print(f'scale {tag}: hall {(OUT / f"hall_{tag}.png").stat().st_size // 1024} KB')
    crest = Image.open(SRC / 'triforce.png').convert('RGBA')
    side = max(crest.size)
    square = Image.new('RGBA', (side, side))
    square.alpha_composite(crest, ((side - crest.width) // 2, (side - crest.height) // 2))
    for n in (64, 32, 16):
        square.resize((n, n), Image.LANCZOS).save(OUT / f'icon_{n}.png', optimize=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
