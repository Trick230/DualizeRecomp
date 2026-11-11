# Fits a free face to the game's Options labels: horizontal scale (press),
# tracking and weight that make it set the game's own English label lines as
# wide and as dark as the game does, at the game's 14-texel capitals.
# python scripts/lettering/font_fit.py <variable .ttf with a wght axis>
import io, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer
import tpl
from glyph_cuts import GAME, LABELS

CAP, S = 14, 8
game = tpl.load(GAME / 'csd' / 'textures' / 'option_allus.tls', 6)[:, :, 0].astype(np.float32) / 255

def ink(a):
    cols = np.where(a.max(0) > 0.12)[0]
    return (cols[-1] - cols[0] + 1, a.sum()) if len(cols) else (0, 0.0)

lines = []
for k, text in enumerate(LABELS["us"]):
    if k not in (0, 1, 3, 5, 7):  # the game squeezes the others to fit
        continue
    w, a = ink(game[24 * k:24 * k + 24])
    lines.append((text, w, a))

def face(weight):
    f = instancer.instantiateVariableFont(TTFont(sys.argv[1]), {'wght': weight})
    b = io.BytesIO(); f.save(b); data = b.getvalue()
    tt = TTFont(io.BytesIO(data))
    size = CAP * tt['head'].unitsPerEm / tt['OS/2'].sCapHeight
    return ImageFont.truetype(io.BytesIO(data), size=size * S)

def render(font, text, press, track):
    im = Image.new('L', (len(text) * 16 * S, 24 * S)); d = ImageDraw.Draw(im)
    x = 4 * S
    for c in text:  # letter by letter for tracking; press below
        d.text((x, 19 * S), c, font=font, fill=255, anchor='ls')
        x += font.getlength(c) + track * S / press
    a = np.asarray(im.resize((int(im.width * press / S), 24), Image.BOX), np.float32) / 255
    return a

best = None
for weight in range(250, 501, 25):
    font = face(weight)
    # width = press * natural + track * (n - 1), solved over the lines
    rows, rhs = [], []
    for text, w, _ in lines:
        nat, _ = ink(render(font, text, 1.0, 0.0))
        rows.append([nat, len(text) - 1]); rhs.append(w)
    (press, track), *_ = np.linalg.lstsq(np.array(rows, float), np.array(rhs, float), rcond=None)
    dark = np.mean([ink(render(font, t, press, track))[1] / a for t, _, a in lines])
    err = np.mean([abs(ink(render(font, t, press, track))[0] - w) for t, w, _ in lines])
    print(weight, 'press', round(press, 3), 'track', round(track, 3), 'ink ratio', round(dark, 3), 'width err', round(err, 2))
    if best is None or abs(dark - 1) < abs(best[0] - 1):
        best = (dark, weight, press, track)
print('best', best)
