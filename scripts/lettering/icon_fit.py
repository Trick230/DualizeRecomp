# Fits the pad-button icons of the game's message font (files/fontJapan, the
# Greek capitals U+0391-039D) with exact shapes: rings, rounded rectangles,
# bars, the d-pad's cross and its notches, and the letters inside drawn by a
# face. Each shape's numbers are fitted to the atlas's cell (24 texels, 4 grey
# levels) by the error per texel of its ink, the shape drawn S times finer and
# averaged down to the atlas's texels.
# python scripts/lettering/icon_fit.py frames   (the rings and rectangles)
import json, sys
import numpy as np
from scipy.optimize import minimize
from atlas_fit import CELL, atlas
from glyph_compose import WORK

S = 16
ICONS = {  # code point: (frame, inside)
    0x391: ('ring', 'A'), 0x392: ('rect', 'B'), 0x393: ('ring', '1'), 0x394: ('ring', '2'),
    0x395: ('ring', '+'), 0x396: ('ring', '-'), 0x397: ('dpad', None), 0x398: ('rect', 'Z'),
    0x399: ('ring', 'B'), 0x39A: ('ring', 'a'), 0x39B: ('ring', 'b'), 0x39C: ('ring', 'x'),
    0x39D: ('ring', 'y'),
}
# The fine grid's sample points, in texels.
_c = (np.arange(CELL * S) + 0.5) / S
X, Y = np.meshgrid(_c, _c)


def down(a):
    return a.reshape(CELL, S, CELL, S).mean((1, 3))


def ring(cx, cy, r, t):
    d = np.hypot(X - cx, Y - cy)
    return (np.abs(d - r) <= t / 2).astype(np.float64)


def rounded_rect(cx, cy, hw, hh, rad, t):
    """A rounded rectangle's outline, `t` thick, centred on its path."""
    rad = max(0.0, min(rad, hw, hh))
    qx = np.abs(X - cx) - (hw - rad)
    qy = np.abs(Y - cy) - (hh - rad)
    d = np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - rad
    return (np.abs(d) <= t / 2).astype(np.float64)


def frame(kind, p):
    return ring(*p) if kind == 'ring' else rounded_rect(*p)


def frame_mask(cell):
    """The atlas's frame alone: its ink outside the middle (the letter's)."""
    return cell


def fit_frame(cell, kind):
    """The frame's numbers: those whose outline best covers the cell's ink that
    lies on it (the letter's ink inside left out by weighting the error by
    distance from the middle)."""
    ys, xs = np.nonzero(cell > 0)
    cx0, cy0 = (xs.min() + xs.max() + 1) / 2, (ys.min() + ys.max() + 1) / 2
    w0, h0 = (xs.max() - xs.min() + 1) / 2, (ys.max() - ys.min() + 1) / 2
    # Only the outer band of the cell's ink: texels nearer the edge of its box.
    yy, xx = np.mgrid[:CELL, :CELL] + 0.5
    if kind == 'ring':
        band = np.abs(np.hypot(xx - cx0, yy - cy0) - (min(w0, h0) - 0.6)) < 2.0
        p0 = [cx0, cy0, min(w0, h0) - 0.6, 1.0]
    else:
        qx, qy = np.abs(xx - cx0) - w0, np.abs(yy - cy0) - h0
        band = np.maximum(qx, qy) > -2.6
        p0 = [cx0, cy0, w0 - 0.6, h0 - 0.6, 1.5, 1.0]
    target = np.where(band, cell, 0)

    def err(p):
        m = down(frame(kind, p))
        return np.abs(np.where(band, m, 0) - target).sum()

    res = minimize(err, p0, method='Powell', options={'xtol': 1e-3, 'ftol': 1e-4, 'maxfev': 4000})
    return list(map(float, res.x)), float(res.fun / max(1e-9, target.sum())), band


if __name__ == '__main__':
    cells = atlas()
    out = {}
    if sys.argv[1] == 'frames':
        for code, (kind, inside) in ICONS.items():
            if kind == 'dpad':
                continue
            p, e, _ = fit_frame(cells[chr(code)], kind)
            out[hex(code)] = {'kind': kind, 'params': p, 'error': e}
            print(hex(code), kind, ' '.join('%.3f' % v for v in p), 'error %.4f' % e, flush=True)
        json.dump(out, open(WORK / 'icon_frames.json', 'w'), indent=1)


# --- The letters inside -------------------------------------------------------
import io
from PIL import Image, ImageDraw, ImageFont
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

ACROSS = [float(k) for k in __import__('os').environ.get('ACROSS', '1').split()]
RING = (12.0, 12.0, 8.55, 1.0)  # fitted above (`frames`): one ring for all


def instance(path, weight):
    f = TTFont(path)
    if 'fvar' in f:
        axes = {a.axisTag: a.defaultValue for a in f['fvar'].axes}
        lo, hi = [(a.minValue, a.maxValue) for a in f['fvar'].axes if a.axisTag == 'wght'][0]
        if not lo <= weight <= hi:
            return None
        axes['wght'] = weight
        f = instancer.instantiateVariableFont(f, axes)
    b = io.BytesIO(); f.save(b); return b.getvalue()


def letter(data, ch, em, across=1.0):
    """The letter at `em` texels (times `across` across), fine grid, its ink
    box's middle on the cell's."""
    size = CELL * S
    font = ImageFont.truetype(io.BytesIO(data), size=em * S)
    im = Image.new('L', (size * 2, size * 2))
    ImageDraw.Draw(im).text((size, size), ch, font=font, fill=255, anchor='ls')
    if across != 1.0:
        w = int(round(size * 2 * across))
        im = im.resize((w, size * 2), Image.BOX if across < 1 else Image.BICUBIC)
        canvas = Image.new('L', (size * 2, size * 2))
        canvas.paste(im, ((size * 2 - w) // 2, 0))
        im = canvas
    a = np.asarray(im, np.float64) / 255
    ys, xs = np.nonzero(a > 0)
    if not len(xs):
        return None
    cy, cx = (ys.min() + ys.max() + 1) // 2, (xs.min() + xs.max() + 1) // 2
    return a[cy - size // 2:cy + size // 2, cx - size // 2:cx + size // 2]


def inside_mask(kind, p):
    yy, xx = np.mgrid[:CELL, :CELL] + 0.5
    if kind == 'ring':
        return np.hypot(xx - p[0], yy - p[1]) < p[2] - 1.2
    return (np.abs(xx - p[0]) < p[2] - 1.2) & (np.abs(yy - p[1]) < p[3] - 1.2)


def fit_letter(cell, mask, data, ch, ems, acrosses=(1.0,)):
    """The letter's em, its width over that and its middle (texels) with the
    least error inside."""
    target = np.where(mask, cell, 0)
    yy, xx = np.mgrid[:CELL, :CELL] + 0.5
    m = target.sum()
    tx, ty = (xx * target).sum() / m, (yy * target).sum() / m
    best = None
    for em, across in [(e, k) for e in ems for k in acrosses]:
        a = letter(data, ch, em, across)
        if a is None:
            return None
        d = down(a)
        ax, ay = (xx * d).sum() / d.sum(), (yy * d).sum() / d.sum()
        sx0, sy0 = int(round((tx - ax) * S)), int(round((ty - ay) * S))
        for sy in range(sy0 - S // 2, sy0 + S // 2 + 1, 2):
            for sx in range(sx0 - S // 2, sx0 + S // 2 + 1, 2):
                e = np.abs(np.where(mask, down(np.roll(np.roll(a, sy, 0), sx, 1)), 0) - target).sum()
                if best is None or e < best[0]:
                    best = (e, em, 12 + sx / S, 12 + sy / S, across)
    return best[0] / m, best[1], best[2], best[3], best[4]


if __name__ == '__main__' and sys.argv[1] == 'letters':
    cells = atlas()
    frames = json.load(open(WORK / 'icon_frames.json'))
    faces = sys.argv[2:]
    weights = [int(w) for w in __import__("os").environ.get("WEIGHTS", "200 250 300 350 400 450 500").split()]
    letters = [(code, inside) for code, (kind, inside) in ICONS.items() if inside and inside not in '+-']
    for path in faces:
        for w in weights:
            data = instance(path, w)
            if data is None:
                continue
            total, rows = 0.0, []
            for code, ch in letters:
                kind = ICONS[code][0]
                p = RING if kind == 'ring' else frames[hex(code)]['params']
                r = fit_letter(cells[chr(code)], inside_mask(kind, p), data, ch,
                               [e / 2 for e in range(20, 45)], ACROSS)
                if r is None:
                    break
                total += r[0]
                rows.append('%s %.3f em %.1f at %.2f,%.2f across %.2f' % (ch, *r))
            else:
                print('%.4f %s wght %d | %s' % (total / len(letters), path.split('/')[-1], w, '; '.join(rows)), flush=True)


# --- Bars, the d-pad -----------------------------------------------------------
def bar(cx, cy, hw, hh):
    return ((np.abs(X - cx) <= hw) & (np.abs(Y - cy) <= hh)).astype(np.float64)


def plus(cx, cy, hl, ht, vl, vt):
    return np.maximum(bar(cx, cy, hl, ht), bar(cx, cy, vt, vl))


def dpad(cx, cy, hl, vl, hw, rad, nd, nl, nt, nx):
    """The cross (arms `hl` across, `vl` down from the middle, `hw` half wide,
    outer corners rounded by `rad`) less a slit on each arm: `nd` from the
    middle, `nl` long, `nt` thick, `nx` off the arm's middle line."""
    def rect(hx, hy):
        r = max(0.0, min(rad, hx, hy))
        qx, qy = np.abs(X - cx) - (hx - r), np.abs(Y - cy) - (hy - r)
        d = np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - r
        return d <= 0
    cross = rect(hl, hw) | rect(hw, vl)
    slit = np.zeros_like(cross)
    for sx, sy in ((0, -1), (0, 1), (-1, 0), (1, 0)):
        if sx == 0:
            mx, my = cx + nx, cy + sy * nd
            slit |= (np.abs(X - mx) <= nt / 2) & (np.abs(Y - my) <= nl / 2)
        else:
            mx, my = cx + sx * nd, cy + nx
            slit |= (np.abs(X - mx) <= nl / 2) & (np.abs(Y - my) <= nt / 2)
    return (cross & ~slit).astype(np.float64)


def fit_shape(cell, fn, p0, mask=None):
    target = cell if mask is None else np.where(mask, cell, 0)

    def err(p):
        m = down(fn(*p))
        return np.abs((m if mask is None else np.where(mask, m, 0)) - target).sum()

    res = minimize(err, p0, method='Powell', options={'xtol': 1e-3, 'ftol': 1e-5, 'maxfev': 6000})
    return list(map(float, res.x)), float(res.fun / target.sum())


if __name__ == '__main__' and sys.argv[1] == 'shapes':
    cells = atlas()
    inside = inside_mask('ring', RING)
    out = {}
    p, e = fit_shape(cells['Ε'], plus, [11.7, 11.5, 4.5, 0.8, 5.0, 0.8], inside)
    out['plus'] = p; print('plus', ' '.join('%.3f' % v for v in p), 'error %.4f' % e)
    p, e = fit_shape(cells['Ζ'], lambda cx, cy, hl, ht: bar(cx, cy, hl, ht), [11.7, 11.5, 4.5, 0.8], inside)
    out['minus'] = p; print('minus', ' '.join('%.3f' % v for v in p), 'error %.4f' % e)
    p, e = fit_shape(cells['Η'], dpad, [11.7, 11.4, 8.5, 8.2, 3.5, 0.5, 5.5, 3.0, 1.0, 0.2])
    out['dpad'] = p; print('dpad', ' '.join('%.3f' % v for v in p), 'error %.4f' % e)
    json.dump(out, open(WORK / 'icon_shapes.json', 'w'), indent=1)


def refine(cell, mask, data, ch, em0, x0, y0, k0):
    """A finer search around a fit: em by 1/8, width by 1/50, place by 1/16."""
    target = np.where(mask, cell, 0)
    m = target.sum()
    best = None
    for em in np.arange(em0 - 0.375, em0 + 0.376, 0.125):
        for k in np.arange(k0 - 0.04, k0 + 0.041, 0.02):
            a = letter(data, ch, float(em), float(k))
            for dy in range(-3, 4):
                for dx in range(-3, 4):
                    sx, sy = int(round((x0 - 12) * S)) + dx, int(round((y0 - 12) * S)) + dy
                    e = np.abs(np.where(mask, down(np.roll(np.roll(a, sy, 0), sx, 1)), 0) - target).sum()
                    if best is None or e < best[0]:
                        best = (e, float(em), 12 + sx / S, 12 + sy / S, float(k))
    return best[0] / m, best[1], best[2], best[3], best[4]


if __name__ == '__main__' and sys.argv[1] == 'refine':
    # refine <face> <weight> <code:em:x:y:across> ...
    cells = atlas()
    frames = json.load(open(WORK / 'icon_frames.json'))
    data = instance(sys.argv[2], int(sys.argv[3]))
    out = {}
    for spec in sys.argv[4:]:
        code, em, x, y, k = spec.split(':')
        code = int(code, 16)
        kind, ch = ICONS[code]
        p = RING if kind == 'ring' else frames[hex(code)]['params']
        r = refine(cells[chr(code)], inside_mask(kind, p), data, ch, float(em), float(x), float(y), float(k))
        out[hex(code)] = r
        print('%s %s error %.4f em %.3f at %.4f,%.4f across %.3f' % (hex(code), ch, *r), flush=True)
    json.dump(out, open(WORK / 'icon_letters.json', 'w'), indent=1)
