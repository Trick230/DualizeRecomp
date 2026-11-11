# Rebuilds Dualize Sans's letters as the game draws them: each letter's
# coverage solved R times finer than a texel from all its copies on the game's
# Options label lines (every language; each copy registered to a sixteenth of
# a texel), Dualize Sans standing in where the copies say little, then traced
# at half coverage into the font. Prints the per-texel error against the
# game's lines before and after. Run after glyph_cuts.py and font_tune.py:
# python scripts/lettering/font_sr.py [rounds]
import io, sys, unicodedata
import numpy as np
from scipy import sparse
from scipy.sparse.linalg import lsqr
from PIL import Image, ImageDraw, ImageFont
from fontTools.ttLib import TTFont
from fontTools.pens.recordingPen import DecomposingRecordingPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from glyph_compose import *
from glyph_compose import ROOT, LABEL_SOURCES

R = 8                 # solved samples per texel
CAP = 14.0
BASE = 19.0
MARGIN = 3.0          # texels left of the origin in a letter's frame
ROWS = 24
FONT = ROOT / 'DualizeRecomp' / 'resources' / 'fonts' / 'DualizeSans.ttf'
data = open(FONT, 'rb').read()
tt = TTFont(io.BytesIO(data))
UPM, CAPH = tt['head'].unitsPerEm, tt['OS/2'].sCapHeight
UNIT = CAPH / CAP     # font units per texel at the game's 14-texel capitals
pil = ImageFont.truetype(io.BytesIO(data), size=CAP * UPM / CAPH * R)
cmap = tt.getBestCmap()

def advance(c):
    return tt['hmtx'][cmap[ord(c)]][0] / UNIT

def prior(c, width):
    """The face's letter in its frame: R per texel, origin MARGIN in, baseline at BASE."""
    im = Image.new('L', (width * R, ROWS * R)); d = ImageDraw.Draw(im)
    d.text((MARGIN * R, BASE * R), c, font=pil, fill=255, anchor='ls')
    return np.asarray(im, np.float64) / 255

def frame_width(c):
    return int(np.ceil(advance(c) + 2 * MARGIN + 2))

def sample_matrix(c, inst, origin, width):
    """Rows: the copy's texels; columns: the frame's samples (box over each texel)."""
    k = inst['cap'] / CAP
    sx, sy = k * inst['press'], k
    a, b = inst['box']
    img = inst['img']
    rows, cols, vals, obs = [], [], [], []
    sub = (np.arange(4) + 0.5) / 4
    n = 0
    for Y in range(ROWS):
        for X in range(a, b):
            idx = []
            for oy in sub:
                v = BASE + (Y + oy - inst['baseline']) / sy
                for ox in sub:
                    u = MARGIN + (X + ox - origin) / sx
                    iu, iv = int(u * R), int(v * R)
                    if 0 <= iu < width * R and 0 <= iv < ROWS * R:
                        idx.append(iv * width * R + iu)
            if not idx:
                continue
            for i in idx:
                rows.append(n); cols.append(i); vals.append(1.0 / 16)
            obs.append(img[Y, X - a]); n += 1
    A = sparse.csr_matrix((vals, (rows, cols)), shape=(n, ROWS * R * width * R))
    return A, np.array(obs)

def register(c, inst, est, width):
    """The copy's origin (line texels) that fits `est` best, to a sixteenth."""
    a, b = inst['box']
    guess = a - (np.where(est.max(0) > 0.3)[0][0] / R - MARGIN) * inst['cap'] / CAP * inst['press']
    best = None
    for d in np.arange(-1.5, 1.5001, 1 / 16):
        A, y = sample_matrix(c, inst, guess + d, width)
        e = float(((A @ est.ravel()) - y) @ ((A @ est.ravel()) - y))
        if best is None or e < best[0]:
            best = (e, guess + d)
    return best[1]

def solve(c, insts, est0, width, lam=0.15, mu=0.6):
    blocks, ys, origins = [], [], []
    for inst in insts:
        o = register(c, inst, est0, width)
        A, y = sample_matrix(c, inst, o, width)
        r = A @ est0.ravel() - y
        w = 1 / np.maximum(1, np.abs(r) / 0.25)  # neighbours' leftovers count less
        blocks.append(sparse.diags(w) @ A); ys.append(w * y); origins.append(o)
    n = ROWS * R * width * R
    eye = sparse.identity(n, format='csr')
    gx = sparse.diags([-1, 1], [0, 1], shape=(n, n), format='csr')
    gy = sparse.diags([-1, 1], [0, width * R], shape=(n, n), format='csr')
    M = sparse.vstack(blocks + [lam * eye, mu * gx, mu * gy])
    rhs = np.concatenate(ys + [lam * est0.ravel(), np.zeros(n), np.zeros(n)])
    h = lsqr(M, rhs, atol=1e-6, btol=1e-6, iter_lim=400)[0]
    return np.clip(h.reshape(ROWS * R, width * R), 0, 1), origins

# ---- marching squares at 0.5, closed loops ----------------------------------
def contours(h):
    H, W = h.shape
    p = np.pad(h, 1)
    segs = {}
    def interp(y0, x0, y1, x1):
        a, b = p[y0, x0], p[y1, x1]
        t = (0.5 - a) / (b - a)
        return (x0 + t * (x1 - x0) - 1, y0 + t * (y1 - y0) - 1)
    for y in range(H + 1):
        for x in range(W + 1):
            q = [p[y, x] > 0.5, p[y, x + 1] > 0.5, p[y + 1, x + 1] > 0.5, p[y + 1, x] > 0.5]
            k = q[0] * 8 + q[1] * 4 + q[2] * 2 + q[3]
            if k in (0, 15):
                continue
            top = lambda: interp(y, x, y, x + 1)
            right = lambda: interp(y, x + 1, y + 1, x + 1)
            bottom = lambda: interp(y + 1, x, y + 1, x + 1)
            left = lambda: interp(y, x, y + 1, x)
            # edges oriented with ink on the right (clockwise outer loops in y-down)
            table = {1: [(bottom, left)], 2: [(right, bottom)], 3: [(right, left)], 4: [(top, right)],
                     5: [(top, left), (bottom, right)], 6: [(top, bottom)], 7: [(top, left)],
                     8: [(left, top)], 9: [(bottom, top)], 10: [(left, bottom), (right, top)],
                     11: [(right, top)], 12: [(left, right)], 13: [(bottom, right)], 14: [(left, bottom)]}
            for s, e in table[k]:
                a, b = s(), e()
                segs[(round(a[0], 6), round(a[1], 6))] = (round(b[0], 6), round(b[1], 6))
    loops = []
    while segs:
        start, nxt = segs.popitem()
        loop = [start]
        while nxt != start and nxt in segs:
            loop.append(nxt); nxt = segs.pop(nxt)
        if len(loop) > 3:
            loops.append(loop)
    return loops

def simplify(loop, tol):
    pts = np.array(loop)
    def dp(a, b):
        seg = pts[a:b + 1]
        if b - a < 2:
            return [a, b]
        d = np.abs(np.cross(pts[b] - pts[a], seg - pts[a])) / (np.hypot(*(pts[b] - pts[a])) + 1e-9)
        i = int(np.argmax(d))
        if d[i] > tol:
            return dp(a, a + i)[:-1] + dp(a + i, b)
        return [a, b]
    idx = dp(0, len(pts) - 1)
    return [tuple(pts[i]) for i in idx[:-1]]

def corners(pts, reach=9, angle=55):
    """Indices where the outline turns sharply (over `reach` samples each way)."""
    n = len(pts); out = []
    for i in range(n):
        a = pts[i] - pts[(i - reach) % n]; b = pts[(i + reach) % n] - pts[i]
        cos = (a @ b) / (np.linalg.norm(a) * np.linalg.norm(b) + 1e-9)
        if np.degrees(np.arccos(np.clip(cos, -1, 1))) > angle:
            out.append(i)
    keep = []  # one per run: the sharpest middle
    for i in out:
        if keep and (i - keep[-1][-1]) <= 2:
            keep[-1].append(i)
        else:
            keep.append([i])
    if len(keep) > 1 and keep[0][0] == 0 and keep[-1][-1] == n - 1:
        keep[0] = keep.pop() + keep[0]
    return [run[len(run) // 2] for run in keep]

def smooth_outline(loop, sigma=2.5, tol=0.6):
    """The traced loop smoothed (corners kept), as (point, on_curve) with the
    curves' points off the outline: a TrueType quadratic spline."""
    pts = np.array(loop, float); n = len(pts)
    pin = corners(pts)
    k = np.exp(-0.5 * (np.arange(-8, 9) / sigma) ** 2); k /= k.sum()
    sm = np.stack([np.convolve(np.concatenate([pts[-8:, d], pts[:, d], pts[:8, d]]), k, 'valid') for d in (0, 1)], 1)
    for i in pin:
        sm[i] = pts[i]
    # between pinned corners, Douglas-Peucker on the smoothed run
    marks = sorted(pin) or [0]
    out = []
    for j, a in enumerate(marks):
        b = marks[(j + 1) % len(marks)]
        run = [sm[(a + t) % n] for t in range(((b - a) % n) or n)]
        idx = dp_indices(np.array(run + [sm[b % n]]), tol)
        out.append((tuple(run[0]), bool(pin)))
        for i in idx[1:-1]:
            out.append((tuple(run[i]), False))
    return out

def dp_indices(pts, tol):
    if np.allclose(pts[0], pts[-1]):  # closed: split at the point farthest from the start
        far = int(np.argmax(np.hypot(*(pts - pts[0]).T)))
        first = dp_indices(pts[:far + 1], tol)
        return first[:-1] + [far + i for i in dp_indices(pts[far:], tol)]
    def dp(a, b):
        if b - a < 2:
            return [a, b]
        seg = pts[a:b + 1]; v = pts[b] - pts[a]
        d = np.abs(v[0] * (seg[:, 1] - pts[a][1]) - v[1] * (seg[:, 0] - pts[a][0])) / (np.hypot(*v) + 1e-9)
        i = int(np.argmax(d))
        return dp(a, a + i)[:-1] + dp(a + i, b) if d[i] > tol else [a, b]
    return dp(0, len(pts) - 1)

def to_units(pt):
    x, y = pt
    return (round((x / R - MARGIN) * UNIT), round((BASE - y / R) * UNIT))

def accent_contours(name, base_name, glyphset):
    """The accented letter's contours not in its base letter (by the base's bounds)."""
    rec = DecomposingRecordingPen(glyphset); glyphset[name].draw(rec)
    base_rec = DecomposingRecordingPen(glyphset); glyphset[base_name].draw(base_rec)
    top = max(pt[1] for op, args in base_rec.value for pt in args if pt is not None and op != 'closePath')
    out, cur = [], []
    for op, args in rec.value:
        cur.append((op, args))
        if op in ('closePath', 'endPath'):
            ys = [pt[1] for o, a in cur for pt in a if pt is not None]
            if ys and (min(ys) > top - 10 or max(ys) < 40):  # above the letter, or a cedilla
                out.append(cur)
            cur = []
    return out

def line_error(fontdata, placements):
    """Mean |coverage - game| over the copies' texels, the face set where each copy was found."""
    f = TTFont(io.BytesIO(fontdata))
    p = ImageFont.truetype(io.BytesIO(fontdata), size=CAP * f['head'].unitsPerEm / f['OS/2'].sCapHeight * R)
    errs = []
    for c, inst, o in placements:
        w = frame_width(c)
        im = Image.new('L', (w * R, ROWS * R)); d = ImageDraw.Draw(im)
        d.text((MARGIN * R, BASE * R), c, font=p, fill=255, anchor='ls')
        est = np.asarray(im, np.float64) / 255
        A, y = sample_matrix(c, inst, o, w)
        errs.append(np.abs(A @ est.ravel() - y).mean())
    return float(np.mean(errs))

if __name__ == '__main__':
    letters = sorted(set(g['ch'] for g in table if g['source'] < LABEL_SOURCES and g['box']
                         and g['ch'].isalpha() and ord(g['ch']) < 128 and g['clean'] < 0.15))
    solved, placements = {}, []
    for c in letters:
        insts = [g for g in table if g['ch'] == c and g['source'] < LABEL_SOURCES and g['box'] and g['clean'] < 0.15]
        w = frame_width(c)
        est = prior(c, w)
        for _ in range(int(sys.argv[1]) if len(sys.argv) > 1 else 2):
            est, origins = solve(c, insts, est, w)
        solved[c] = est
        placements += [(c, i, o) for i, o in zip(insts, origins)]
        print(c, len(insts), end='  ', flush=True)
    print()
    before = line_error(data, placements)
    # into the font
    f = TTFont(io.BytesIO(data)); gs = f.getGlyphSet(); glyf = f['glyf']
    new = {}
    for c, h in solved.items():
        pen = TTGlyphPen(None)
        for loop in contours(h):
            if len(loop) < 8:
                continue
            outline = [(to_units(p), on) for p, on in smooth_outline(loop)]
            if not any(on for _, on in outline):  # no corner: all off the curve
                pen.qCurveTo(*[p for p, _ in outline], None)
                pen.closePath()
                continue
            first = next(i for i, (_, on) in enumerate(outline) if on)
            outline = outline[first:] + outline[:first]
            pen.moveTo(outline[0][0])
            offs = []
            for p, on in outline[1:] + outline[:1]:
                if not on:
                    offs.append(p)
                elif offs:
                    pen.qCurveTo(*offs, p); offs = []
                else:
                    pen.lineTo(p)
            pen.closePath()
        new[cmap[ord(c)]] = pen.glyph()
    for u, name in cmap.items():  # accented letters: the new base and their own accents
        d = unicodedata.normalize('NFD', chr(u))
        if len(d) == 2 and d[0] in solved and name not in new:
            pen = TTGlyphPen(None)
            rec = DecomposingRecordingPen(None); new[cmap[ord(d[0])]].draw(rec, glyf)
            xh = f['OS/2'].sxHeight
            cur = []
            for op, args in rec.value:  # an i's dot gives way to the accent
                cur.append((op, args))
                if op in ('closePath', 'endPath'):
                    if not (d[0] == 'i' and min(pt[1] for o, a in cur for pt in a if pt is not None) > xh):
                        for o, a in cur:
                            getattr(pen, o)(*a)
                    cur = []
            for contour in accent_contours(name, cmap[ord(d[0])], gs):
                for op, args in contour:
                    getattr(pen, op)(*args)
            new[name] = pen.glyph()
    for name, g in new.items():
        g.recalcBounds(glyf); glyf[name] = g
        adv, _ = f['hmtx'][name]
        f['hmtx'][name] = (adv, g.xMin if g.numberOfContours else 0)
    b = io.BytesIO(); f.save(b); out = b.getvalue()
    after = line_error(out, placements)
    open(FONT, 'wb').write(out)
    print('copies', len(placements), 'error per texel: before', round(before, 4), 'after', round(after, 4))
