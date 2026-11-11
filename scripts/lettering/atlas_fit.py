# Scores faces against the game's message font (files/fontJapan, 24-texel
# cells, 4 grey levels): each glyph drawn at an em size, registered on the
# atlas's to a quarter texel, its error per texel over the cell's ink; the
# em size that fits best, per face (and weight, for variable faces).
# python scripts/lettering/atlas_fit.py <kana|kanji|latin> <.ttf> [...]
import io, re, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer
from glyph_cuts import GAME

CELL, S = 24, 4
LV = np.array([0, 98, 189, 255], np.float64) / 255

def atlas(name='fontJapan'):
    b = np.frombuffer(open(GAME / (name + '.bin'), 'rb').read(), np.uint8)
    inf = open(GAME / (name + '.inf'), encoding='latin1').read()
    w, h = map(int, re.search(r'bitmapSize\s*:\s*(\d+)\s+(\d+)', inf).groups())
    img = LV[np.stack([(b >> (2 * k)) & 3 for k in range(4)], 1).ravel()[:w * h]].reshape(h, w)
    cells = {}
    for hw, hh, cx, cy, u in re.findall(r'index :\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s*// 0x[0-9a-f]+ unicode 0x([0-9a-f]+)', inf):
        cx, cy = int(cx), int(cy)
        cell = img[cy - CELL // 2:cy + CELL // 2, cx - CELL // 2:cx + CELL // 2]
        if cell.shape == (CELL, CELL):
            cells[chr(int(u, 16))] = cell
    return cells

SETS = {
    'kana': 'あいうえおかきくけこさしすせそたちつてとなにぬねのはひふへほまみむめもやゆよらりるれろわをんがぎぐげござじずぜぞアイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホマミムメモヤユヨラリルレロワヲン',
    'kanji': '夢見世界人日本時間中大小上下出入行来子女男手足目口心気分前後年月火水木金土空風海山川光闇力友話言語',
    'latin': 'ABCDEFGHIJKLMNOPRSTUVWYabcdefghijklmnoprstuvwyz0123456789',
}

def face_data(path, weight):
    f = TTFont(path)
    if 'fvar' in f and weight:
        axes = {a.axisTag: a.defaultValue for a in f['fvar'].axes}; axes['wght'] = weight
        f = instancer.instantiateVariableFont(f, axes)
    b = io.BytesIO(); f.save(b); return b.getvalue()

def render(font, c):
    im = Image.new('L', (CELL * 2 * S, CELL * 2 * S)); d = ImageDraw.Draw(im)
    d.text((CELL * S, CELL * S), c, font=font, fill=255, anchor='mm')
    a = np.asarray(im.resize((CELL * 2, CELL * 2), Image.BOX), np.float64) / 255
    return a

def shift(a, dx, dy):
    from scipy import ndimage
    return ndimage.shift(a, (dy, dx), order=1)

def score(cells, data, em, chars):
    font = ImageFont.truetype(io.BytesIO(data), size=em * S)
    errs = []
    for c in chars:
        if c not in cells:
            continue
        g = cells[c]
        r = render(font, c)
        # centroids aligned, then a quarter-texel refine
        def cen(a):
            ys, xs = np.mgrid[:a.shape[0], :a.shape[1]]; m = a.sum() + 1e-9
            return (xs * a).sum() / m, (ys * a).sum() / m
        gx, gy = cen(g); rx, ry = cen(r)
        best = None
        for ddx in np.arange(-0.5, 0.51, 0.25):
            for ddy in np.arange(-0.5, 0.51, 0.25):
                s = shift(r, gx - rx + ddx + 0, gy - ry + ddy)[:CELL, :CELL]
                e = np.abs(s - g).sum() / max(1.0, g.sum())
                if best is None or e < best:
                    best = e
        errs.append(best)
    return float(np.mean(errs))

if __name__ == '__main__':
    cells = atlas()
    chars = SETS[sys.argv[1]]
    for path in sys.argv[2:]:
        f = TTFont(path)
        weights = [600, 650, 700, 750, 800] if "fvar" in f else [None]
        res = []
        for w in weights:
            data = face_data(path, w)
            for em in (19, 19.5, 20, 20.5, 21):
                res.append((score(cells, data, em, chars), w, em))
        res.sort()
        print(round(res[0][0], 4), path.split('/')[-1], 'weight', res[0][1], 'em', res[0][2], flush=True)
