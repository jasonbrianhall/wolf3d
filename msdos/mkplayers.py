#!/usr/bin/env python3
"""
mkplayers.py -- build PLAYERS.WL6 (the other players' sprites in a co-op
game) from a sprite sheet laid out like "Wolfenstein 3D New BJ Blazkowicz
Sprites" by AndersonMiguel1105 (DeviantArt):

    row 0       standing, 8 directions          (columns 0-7)
    rows 1-4    walking frames 1-4, 8 directions
    row 5       firing (muzzle flash), 8 directions
    row 6       aiming, 8 directions
    column 8    rows 1-6: pain, dying 1-4, dead (row 0 and column 9,
                the gibs, aren't used)

The directions are in the game's order: 0 faces the viewer, 2 faces left,
4 faces away, 6 faces right. The background must be transparent.

    python3 mkplayers.py sheet.png PLAYERS.WL6 [--height 58] [--preview p.png]

Put the file next to wolf3d.exe (PLAYERS.WL1 / .SOD / .SDM for the other
versions: the same file renamed). Without it the other players are the
recolored SS, as before.

The file:
    char    magic[4]        "WLPS"
    word    version         1
    word    numframes       (NUMFRAMES below)
    byte    team[256]       1 for the palette colors recolored per player
    long    offset[numframes], word length[numframes]
    ...     frames: t_compshape, like the sprites in VSWAP
"""

import argparse, os, re, struct, sys
import numpy as np
from PIL import Image

# frame numbers in the file (wl_net.c: PF_*)
PF_STAND, PF_WALK, PF_AIM, PF_FIRE = 0, 8, 40, 48
PF_PAIN, PF_DIE1, PF_DEAD = 56, 57, 61
NUMFRAMES = 62

GRIDROWS = 7
GRIDCOLS = 8
SHEETROW_FOR = {0: PF_STAND, 1: PF_WALK, 2: PF_WALK + 8, 3: PF_WALK + 16,
                4: PF_WALK + 24, 5: PF_FIRE, 6: PF_AIM}


def load_palette(path):
    src = open(path).read()
    nums = [int(n) for n in re.findall(r'\d+', src.split('{', 1)[1])]
    pal = np.array(nums[:768], dtype=np.int32).reshape(256, 3)
    return pal * 255 // 63


def runs(mask1d):
    out, start = [], None
    for i, v in enumerate(mask1d):
        if v and start is None:
            start = i
        elif not v and start is not None:
            out.append((start, i))
            start = None
    if start is not None:
        out.append((start, len(mask1d)))
    return out


def cut_sheet(rgba):
    """{frame: RGBA crop} cut along the gaps between the figures"""
    alpha = rgba[:, :, 3] >= 128
    H, W = alpha.shape

    # the 8 direction columns end where the death column starts: the widest
    # empty gap in the right part of the sheet
    colprof = alpha.sum(0)
    gaps = [r for r in runs(colprof == 0) if r[0] > W // 2]
    gridright = max(gaps, key=lambda r: r[1] - r[0])[0] if gaps else W * 3 // 4
    deathleft = max(gaps, key=lambda r: r[1] - r[0])[1] if gaps else gridright

    rowruns = [r for r in runs(alpha[:, :gridright].sum(1) > 0) if r[1] - r[0] > 20]
    if len(rowruns) != GRIDROWS:
        sys.exit("expected %d rows of figures, found %d" % (GRIDROWS, len(rowruns)))

    frames = {}
    for row, (y0, y1) in enumerate(rowruns):
        prof = alpha[y0:y1, :gridright].sum(0)
        # figure centers: the 8 heaviest runs, then split between them at
        # the last lowest point (a muzzle flash reaches toward the next one)
        centers = []
        for a, b in sorted(runs(prof > 8), key=lambda r: -prof[r[0]:r[1]].sum())[:GRIDCOLS]:
            centers.append((a + b) // 2)
        centers.sort()
        if len(centers) != GRIDCOLS:
            sys.exit("row %d: expected %d figures, found %d" % (row, GRIDCOLS, len(centers)))
        bounds = [0]
        for c0, c1 in zip(centers, centers[1:]):
            seg = prof[c0:c1]
            lo = seg.min()
            bounds.append(c0 + max(i for i, v in enumerate(seg) if v == lo))
        bounds.append(gridright)
        for col in range(GRIDCOLS):
            crop = rgba[y0:y1, bounds[col]:bounds[col + 1]].copy()
            frames[SHEETROW_FOR[row] + col] = ('head', crop)

    # the death column: its figures top to bottom. It touches the gibs'
    # column, so the figures are told apart by where their middles are
    from scipy import ndimage
    lab, n = ndimage.label(ndimage.binary_dilation(alpha[:, deathleft:], iterations=1))
    blobs = []
    for i, sl in enumerate(ndimage.find_objects(lab)):
        if (sl[0].stop - sl[0].start) * (sl[1].stop - sl[1].start) > 40:
            blobs.append((i + 1, sl))
    mids = [(sl[1].start + sl[1].stop) / 2 for _, sl in blobs]
    split = (min(mids) + max(mids)) / 2
    keep = np.zeros_like(alpha)
    for (i, sl), mid in zip(blobs, mids):
        if mid < split:
            keep[:, deathleft:] |= (lab == i) & alpha[:, deathleft:]
    rgba = rgba.copy()
    deathcol = rgba.copy()
    deathcol[~keep] = 0
    xs = np.nonzero(keep.any(0))[0]
    deathright = xs.max() + 1
    figs = [r for r in runs(keep.sum(1) > 0) if r[1] - r[0] > 4]
    # skip the standing one at the top; pain, die 1-4 (the last is dead)
    figs = figs[1:7]
    if len(figs) != 6:
        sys.exit("death column: expected 7 figures, found %d" % (len(figs) + 1))
    for i, (y0, y1) in enumerate(figs):
        crop = deathcol[y0:y1, deathleft:deathright].copy()
        frames[PF_PAIN + i] = ('head' if i == 0 else 'box', crop)
    return frames


def place(crop, scale, anchor):
    """the figure on a 64x64 canvas: feet on the bottom row, centered"""
    img = Image.fromarray(crop, 'RGBA')
    # resample premultiplied, so the transparent edges don't go dark
    a = np.array(img).astype(np.float32)
    a[:, :, :3] *= a[:, :, 3:4] / 255.0
    w = max(1, round(img.width * scale))
    h = max(1, round(img.height * scale))
    big = np.array(Image.fromarray(a.astype(np.uint8), 'RGBA').resize((w, h), Image.BICUBIC)).astype(np.float32)
    al = big[:, :, 3:4]
    big[:, :, :3] = np.where(al > 0, big[:, :, :3] * 255.0 / np.maximum(al, 1), 0)
    big = np.clip(big, 0, 255).astype(np.uint8)

    opaque = big[:, :, 3] >= 128
    ys, xs = np.nonzero(opaque)
    if not len(ys):
        return big, opaque
    top, bottom = ys.min(), ys.max()
    if anchor == 'head':
        head = opaque[top:top + max(4, round(10 * scale))]
        hx = np.nonzero(head)[1]
        cx = (hx.min() + hx.max()) / 2
    else:
        cx = (xs.min() + xs.max()) / 2

    canvas = np.zeros((64, 64, 4), np.uint8)
    dx = int(round(32 - cx))
    dy = 63 - bottom
    for y, x in zip(ys, xs):
        nx, ny = x + dx, y + dy
        if 0 <= nx < 64 and 0 <= ny < 64:
            canvas[ny, nx] = big[y, x]
    return canvas, canvas[:, :, 3] >= 128


# The vest: Wolf's palette has bright golds (161-165) but its mid tones are
# olive (72-79) or brown, so straight nearest-color makes it lime green.
# Vest pixels are matched by brightness on this ramp instead, and the ramp
# is exactly what gets recolored per player (nothing else may use it).
VESTRAMP = [223, 222, 220, 218, 6, 76, 75, 74, 165, 164, 163, 162, 161]


def is_vest(rgb):
    """saturated gold: not the tan hair (hue < 36), the skin, or the
    orange muzzle flash (hue 30)"""
    f = rgb.astype(np.float32) / 255
    mx, mn = f.max(1), f.min(1)
    sat = np.where(mx > 0, (mx - mn) / np.maximum(mx, 1e-6), 0)
    r, g, b = f[:, 0], f[:, 1], f[:, 2]
    d = np.maximum(mx - mn, 1e-6)
    hue = np.where(mx == r, 60 * ((g - b) / d % 6), np.where(mx == g, 60 * ((b - r) / d + 2), 60 * ((r - g) / d + 4)))
    return (hue >= 36) & (hue <= 64) & (sat >= 0.65) & (mx >= 0.2)


def luma(rgb):
    return rgb[..., 0] * 0.299 + rgb[..., 1] * 0.587 + rgb[..., 2] * 0.114


def lab(rgb):
    """sRGB 0-255 -> CIELAB, for a nearest color that keeps the hue"""
    c = rgb.astype(np.float64) / 255
    c = np.where(c > 0.04045, ((c + 0.055) / 1.055) ** 2.4, c / 12.92)
    m = np.array([[0.4124, 0.3576, 0.1805], [0.2126, 0.7152, 0.0722], [0.0193, 0.1192, 0.9505]])
    xyz = c @ m.T / np.array([0.9505, 1.0, 1.089])
    f = np.where(xyz > 0.008856, np.cbrt(xyz), 7.787 * xyz + 16 / 116)
    return np.stack([116 * f[..., 1] - 16, 500 * (f[..., 0] - f[..., 1]), 200 * (f[..., 1] - f[..., 2])], -1)


def quantize(rgb, pal, vest):
    out = np.zeros(len(rgb), np.uint8)
    if vest.any():
        ramp = np.array(VESTRAMP)
        rl = luma(pal[ramp].astype(np.float32))
        out[vest] = ramp[np.abs(luma(rgb[vest].astype(np.float32))[:, None] - rl[None, :]).argmin(1)]
    rest = ~vest
    if rest.any():
        # never the vest's ramp (it changes color per player), never 255
        allowed = np.array([i for i in range(255) if i not in VESTRAMP])
        d = ((lab(rgb[rest])[:, None, :] - lab(pal[allowed])[None, :, :]) ** 2).sum(2)
        out[rest] = allowed[d.argmin(1)]
    return out


def compshape(pixels, mask):
    """t_compshape: leftpix, rightpix, dataofs[], pixel data, posts"""
    cols = [x for x in range(64) if mask[:, x].any()]
    if not cols:
        cols = [32]
    left, right = cols[0], cols[-1]
    n = right - left + 1
    hdr = 4 + 2 * n
    pix = bytearray()
    posts = []
    for x in range(left, right + 1):
        col = []
        for y0, y1 in runs(mask[:, x]):
            start = hdr + len(pix)
            pix += bytes(pixels[y0:y1, x])
            col.append((y1 * 2, (start - y0) & 0xffff, y0 * 2))
        posts.append(col)
    data = bytearray()
    offs = []
    base = hdr + len(pix)
    for col in posts:
        offs.append(base + len(data))
        for end, top, start in col:
            data += struct.pack('<HHH', end, top, start)
        data += struct.pack('<H', 0)
    out = struct.pack('<HH', left, right) + b''.join(struct.pack('<H', o) for o in offs) + pix + data
    assert len(out) < 0x10000
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('sheet')
    ap.add_argument('out')
    ap.add_argument('--height', type=float, default=58, help='standing height in the 64x64 sprite (default 58)')
    ap.add_argument('--palette', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'wolfsrc', 'gamepal.c'))
    ap.add_argument('--preview', help='also write the frames as a PNG contact sheet')
    args = ap.parse_args()

    pal = load_palette(args.palette)
    rgba = np.array(Image.open(args.sheet).convert('RGBA'))
    cuts = cut_sheet(rgba)

    stand_h = np.mean([(cuts[PF_STAND + i][1][:, :, 3] >= 128).any(1).sum() for i in range(8)])
    scale = args.height / stand_h

    pix, masks = [], []
    for f in range(NUMFRAMES):
        anchor, crop = cuts[f]
        canvas, m = place(crop, scale, anchor)
        p = np.zeros((64, 64), np.uint8)
        vest = is_vest(canvas[:, :, :3].reshape(-1, 3)).reshape(64, 64)
        if anchor == 'head' and m.any():
            # blond hair is as gold as the vest: the head is never vest
            top = np.nonzero(m.any(1))[0].min()
            vest[:top + int(round(13 * scale))] = False
        p[m] = quantize(canvas[:, :, :3][m], pal, vest[m])
        pix.append(p)
        masks.append(m)

    team = [1 if i in VESTRAMP else 0 for i in range(256)]
    blobs = [compshape(p, m) for p, m in zip(pix, masks)]

    hdr = b'WLPS' + struct.pack('<HH', 1, NUMFRAMES) + bytes(team)
    off = len(hdr) + NUMFRAMES * 6
    table_o, table_l = b'', b''
    for b in blobs:
        table_o += struct.pack('<l', off)
        table_l += struct.pack('<H', len(b))
        off += len(b)
    with open(args.out, 'wb') as f:
        f.write(hdr + table_o + table_l + b''.join(blobs))
    print('%s: %d frames, %d bytes, scale %.2f, %d team colors'
          % (args.out, NUMFRAMES, off, scale, sum(team)))

    if args.preview:
        # every frame, and the 4 player colors of the standing frames
        tints = [lambda r, g, b: (b, (r + g) // 2, b), lambda r, g, b: ((r + g) // 2, b, b),
                 lambda r, g, b: (r, g, b), lambda r, g, b: ((r + g) // 2,) * 3]
        sheet = Image.new('RGB', (64 * 16, 64 * 6), (40, 60, 40))
        for f in range(NUMFRAMES):
            im = np.zeros((64, 64, 3), np.uint8) + (40, 60, 40)
            im[masks[f]] = pal[pix[f][masks[f]]]
            sheet.paste(Image.fromarray(im.astype(np.uint8)), ((f % 16) * 64, (f // 16) * 64))
        for p in range(4):
            for d in range(8):
                im = np.zeros((64, 64, 3), np.int32) + (40, 60, 40)
                m = masks[PF_STAND + d]
                for y, x in zip(*np.nonzero(m)):
                    c = pix[PF_STAND + d][y, x]
                    rgb = pal[c]
                    if team[c]:
                        rgb = tints[p](*rgb)
                    im[y, x] = rgb
                sheet.paste(Image.fromarray(im.astype(np.uint8)), ((8 * (p % 2) + d) * 64, (4 + p // 2) * 64))
        sheet.resize((sheet.width * 2, sheet.height * 2), Image.NEAREST).save(args.preview)


if __name__ == '__main__':
    main()
