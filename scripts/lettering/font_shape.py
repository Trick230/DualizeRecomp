# Compares the face with the game's own Options letters, letter by letter, at
# the game's 14-texel capitals: ink width, ink height (x-height for small
# letters), side bearings. Writes build/lettering/font_shape.json.
# python scripts/lettering/font_shape.py <the face's .ttf>
import io, sys, json
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from fontTools.ttLib import TTFont
from glyph_compose import *
from glyph_compose import WORK

CAP, S = 14, 8
data = open(sys.argv[1], 'rb').read()
tt = TTFont(io.BytesIO(data))
font = ImageFont.truetype(io.BytesIO(data), size=CAP * tt['head'].unitsPerEm / tt['OS/2'].sCapHeight * S)

def box(a):
    cols = np.where(a.max(0) > 0.3)[0]; rows = np.where(a.max(1) > 0.3)[0]
    return (cols[0], cols[-1] + 1, rows[0], rows[-1] + 1) if len(cols) else None

def face(c):
    im = Image.new('L', (40 * S, 24 * S)); d = ImageDraw.Draw(im)
    d.text((4 * S, 19 * S), c, font=font, fill=255, anchor='ls')
    a = np.asarray(im, np.float32) / 255
    b = box(a)
    adv = font.getlength(c) / S
    return ((b[1] - b[0]) / S, (b[3] - b[2]) / S, b[0] / S - 4, adv - (b[1] / S - 4))

out = {}
for c in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPRSTVWY':
    if not pick(c, CAP):
        continue
    g = glyph(c, CAP, 1.0)
    b = box(g)
    if b is None:
        continue
    gw, gh = b[1] - b[0], b[3] - b[2]
    fw, fh, fl, fr = face(c)
    key = c if c in LSB else None
    out[c] = dict(game_w=float(gw), game_h=float(gh), face_w=fw, face_h=fh,
                  game_l=LSB[key] * CAP if key else None, game_r=RSB[key] * CAP if key else None, face_l=fl, face_r=fr)
json.dump(out, open(WORK / 'font_shape.json', 'w'), indent=1)
w = np.array([v['game_w'] / v['face_w'] for v in out.values()])
xs = [out[c]['game_h'] / out[c]['face_h'] for c in 'acemnorsuvwxz' if c in out]
caps = [out[c]['game_h'] / out[c]['face_h'] for c in 'EFHILT' if c in out]
print('letters', len(out), 'width ratio median', round(float(np.median(w)), 3), 'spread', round(float(w.std()), 3))
print('x-height ratio', round(float(np.median(xs)), 3), 'cap ratio', round(float(np.median(caps)), 3))
print('widest off:', sorted(((round(v['game_w'] / v['face_w'], 2), c) for c, v in out.items()))[:5], sorted(((round(v['game_w'] / v['face_w'], 2), c) for c, v in out.items()))[-5:])
