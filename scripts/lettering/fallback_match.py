# Scores free faces (and weights) against the game's own letters of the
# Options labels' face, letter by letter: rendered at cap 14, aligned on the
# baseline and the ink's left edge, mean absolute coverage difference.
# python scripts/lettering/fallback_match.py <folder of .ttf>  (after glyph_cuts.py)
import io, glob
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer
import sys
from glyph_compose import *

LETTERS = [c for c in 'acdeghilmnorstuvwbkpACEFLRSW' if pick(c, 14)]
game = {}
for c in LETTERS:
    im = glyph(c, 14, 1.0)
    cols = np.where(im.max(0) > 0.12)[0]
    game[c] = im[:, cols[0]:cols[-1] + 1]

def render(data, c, cap):
    tt = TTFont(io.BytesIO(data))
    size = cap * tt['head'].unitsPerEm / tt['OS/2'].sCapHeight
    S = 4
    f = ImageFont.truetype(io.BytesIO(data), size=size * S)
    im = Image.new('L', (40 * S, 24 * S)); d = ImageDraw.Draw(im)
    d.text((4 * S, 19 * S), c, font=f, fill=255, anchor='ls')
    a = np.asarray(im.resize((40, 24), Image.BOX), np.float32) / 255
    cols = np.where(a.max(0) > 0.12)[0]
    return a[:, cols[0]:cols[-1] + 1] if len(cols) else a[:, :1]

def score(data):
    errs = []
    for c, g in game.items():
        r = render(data, c, 14)
        w = max(g.shape[1], r.shape[1])
        gp = np.pad(g, ((0, 0), (0, w - g.shape[1]))); rp = np.pad(r, ((0, 0), (0, w - r.shape[1])))
        errs.append(np.abs(gp - rp).sum() / max(1.0, g.sum()))
    return float(np.mean(errs))

results = []
for path in sorted(glob.glob(sys.argv[1] + '/*.ttf')):
    if '/jp_' in path.replace('\\', '/'):
        continue
    tt = TTFont(path)
    axes = {a.axisTag: (a.minValue, a.maxValue) for a in tt['fvar'].axes} if 'fvar' in tt else {}
    weights = range(int(max(200, axes['wght'][0])), int(min(500, axes['wght'][1])) + 1, 50) if 'wght' in axes else [None]
    for wg in weights:
        if wg is None:
            data = open(path, 'rb').read()
        else:
            f = instancer.instantiateVariableFont(TTFont(path), {'wght': wg}); b = io.BytesIO(); f.save(b); data = b.getvalue()
        results.append((score(data), path.split('\\')[-1].split('/')[-1], wg))
results.sort()
for r in results[:8]:
    print(round(r[0], 3), r[1], r[2])
