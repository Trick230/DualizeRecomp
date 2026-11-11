# Fits each letter's width to the game's: Dualize Sans's letter (as
# font_tune.py builds it) widened or narrowed about its middle, the factor that
# best fits all its copies on the game's label lines (each copy registered to
# an eighth of a texel), drawn towards 1 for letters with few copies. The
# outlines stay the face's, so the letters stay alike. Writes
# build/lettering/glyph_widths.json for font_tune.py, prints the error per
# texel before and after.
# python scripts/lettering/font_widths.py <KumbhSans variable .ttf>
import io, json, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from fontTools.ttLib import TTFont
from glyph_compose import table, LABEL_SOURCES, WORK
import font_sr
import font_tune

R, CAP = font_sr.R, font_sr.CAP
MAX_COPIES = 24

def face(data):
    tt = TTFont(io.BytesIO(data))
    return ImageFont.truetype(io.BytesIO(data), size=CAP * tt['head'].unitsPerEm / tt['OS/2'].sCapHeight * R), tt

def letter(pil, c, width):
    im = Image.new('L', (width * R, font_sr.ROWS * R)); d = ImageDraw.Draw(im)
    d.text((font_sr.MARGIN * R, font_sr.BASE * R), c, font=pil, fill=255, anchor='ls')
    return np.asarray(im, np.float64) / 255

def widen(est, k):
    cols = np.where(est.max(0) > 0.5)[0]
    mid = (cols[0] + cols[-1]) / 2
    x = np.arange(est.shape[1])
    src = mid + (x - mid) / k
    out = np.zeros_like(est)
    for r in range(est.shape[0]):
        out[r] = np.interp(src, x, est[r], left=0, right=0)
    return out

def error(c, insts, est, width):
    total, n = 0.0, 0
    for inst in insts:
        o0 = font_sr.register(c, inst, est, width) if 'o' not in inst else inst['o']
        best = None
        for d in np.arange(-0.25, 0.2501, 0.125):
            A, y = font_sr.sample_matrix(c, inst, o0 + d, width)
            e = float(np.abs(A @ est.ravel() - y).sum())
            if best is None or e < best[0]:
                best = (e, len(y))
        total += best[0]; n += best[1]
    return total / n

if __name__ == '__main__':
    weight = float(sys.argv[2]) if len(sys.argv) > 2 else 350
    data = font_tune.build(weight)
    pil, tt = face(data)
    font_sr.tt = tt; font_sr.cmap = tt.getBestCmap(); font_sr.UNIT = tt['OS/2'].sCapHeight / CAP
    letters = sorted(set(g['ch'] for g in table if g['source'] < LABEL_SOURCES and g['box'] and g['ch'].isalpha()
                         and ord(g['ch']) < 128 and g['clean'] < 0.15))
    out, before, after, weights = {}, [], [], []
    for c in letters:
        insts = [g for g in table if g['ch'] == c and g['source'] < LABEL_SOURCES and g['box'] and g['clean'] < 0.15]
        insts = insts[:MAX_COPIES]
        width = font_sr.frame_width(c)
        base = letter(pil, c, width)
        for inst in insts:
            inst.pop('o', None)
            inst['o'] = font_sr.register(c, inst, base, width)
        scores = {k: error(c, insts, widen(base, k), width) for k in np.round(np.arange(0.80, 1.1201, 0.02), 2)}
        best = min(scores, key=scores.get)
        n = len(insts)
        factor = 1 + (best - 1) * n / (n + 4)
        out[c] = round(float(factor), 3)
        before.append(scores[1.0]); after.append(scores[min(scores, key=lambda k: abs(k - factor))]); weights.append(n)
        print(f'{c}{n}:{out[c]}', end=' ', flush=True)
    print()
    json.dump(out, open(WORK / "glyph_widths.json", "w"), indent=1)
    w = np.array(weights, float)
    print('error per texel: before', round(float(np.average(before, weights=w)), 4), 'after', round(float(np.average(after, weights=w)), 4))
