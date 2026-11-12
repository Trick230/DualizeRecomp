# Fits where a face's baseline sits in the game's message font cells
# (files/fontJapan, 24-texel cells): every glyph drawn at the em size the
# runtime uses, its ink centred across on the cell's middle (the window the
# game draws is symmetric about it) and its baseline at B texels from the
# cell's top, one B for the whole face; the B with the least error per texel
# of the atlas's ink. Also prints, per glyph, the shifts that would fit it best
# alone, to show how evenly the atlas sets its glyphs.
# The em down may differ from the one across (the em sizes the widths were
# fitted at): with `--down`, the em down is fitted too.
# python scripts/lettering/baseline_fit.py <latin|kana|kanji> <.ttf> <em> [--down]
import io, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from atlas_fit import CELL, SETS, atlas

S = 16  # sub-texel steps


def render(font, c, base, stretch=1.0):
    """The glyph at S times the atlas's size (`font` at the em down, `stretch`
    the em across over it), ink centred across, baseline at `base` texels;
    None when it has no ink."""
    size = CELL * S
    im = Image.new('L', (size * 2, size * 2))
    ImageDraw.Draw(im).text((size, size), c, font=font, fill=255, anchor='ls')
    if stretch != 1.0:
        w = int(round(size * 2 * stretch))
        im = im.resize((w, size * 2), Image.BOX if stretch < 1 else Image.BICUBIC)
        canvas = Image.new('L', (size * 2, size * 2))
        canvas.paste(im, ((size * 2 - w) // 2, 0))
        im = canvas
    a = np.asarray(im, np.float64) / 255
    cols = np.nonzero(a.any(0))[0]
    if not len(cols):
        return None
    mid = (cols[0] + cols[-1] + 1) / 2
    out = np.zeros((size, size))
    dx = int(round(size / 2 - mid))
    dy = int(round(base * S - size))
    ys, xs = np.nonzero(a)
    ys2, xs2 = ys + dy, xs + dx
    ok = (ys2 >= 0) & (ys2 < size) & (xs2 >= 0) & (xs2 < size)
    out[ys2[ok], xs2[ok]] = a[ys[ok], xs[ok]]
    return out


def down(a):
    return a.reshape(CELL, S, CELL, S).mean((1, 3))


def error(r, g, dx=0, dy=0):
    r = np.roll(np.roll(r, dy, 0), dx, 1)
    return np.abs(down(r) - g).sum()


def fit(drawn, cells, ink):
    """The face's one shift down (in 1/S texels) with the least error."""
    best = []
    for dy in range(-2 * S, 2 * S + 1):
        best.append((sum(error(r, cells[c], 0, dy) for c, r in drawn.items()) / ink, dy))
    return min(best)


if __name__ == '__main__':
    cells = atlas()
    chars = [c for c in SETS[sys.argv[1]] if c in cells]
    data = open(sys.argv[2], 'rb').read()
    em = float(sys.argv[3])
    ink = sum(cells[c].sum() for c in chars)
    base0 = 18.0
    downs = [em + k * 0.25 for k in range(-4, 9)] if '--down' in sys.argv else [em]
    results = []
    for em_down in downs:
        font = ImageFont.truetype(io.BytesIO(data), size=em_down * S)
        drawn = {c: render(font, c, base0, em / em_down) for c in chars}
        drawn = {c: r for c, r in drawn.items() if r is not None}
        e, dy = fit(drawn, cells, ink)
        results.append((e, em_down, dy, drawn))
        print('em down %.2f: baseline %.4f texels, error per ink texel %.4f' % (em_down, base0 + dy / S, e), flush=True)
    e, em_down, dy, drawn = min(results, key=lambda r: r[0])
    print('best: em down %.2f, baseline %.4f, error %.4f' % (em_down, base0 + dy / S, e))
    # Each glyph alone: its best shift across and down around the face's.
    alone = []
    for c, r in drawn.items():
        g = cells[c]
        cand = [(error(r, g, ddx, dy + ddy), ddx, ddy) for ddy in range(-S, S + 1, 2) for ddx in range(-S, S + 1, 2)]
        cand.sort()
        alone.append((c, cand[0][1] / S, cand[0][2] / S))
    xs = np.array([a[1] for a in alone]); ys = np.array([a[2] for a in alone])
    print('alone across: mean %.3f sd %.3f; down: mean %.3f sd %.3f texels' % (xs.mean(), xs.std(), ys.mean(), ys.std()))
    print(' '.join('%s%+.2f/%+.2f' % a for a in alone))
