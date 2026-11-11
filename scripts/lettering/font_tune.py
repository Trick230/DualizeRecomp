# Bends Kumbh Sans towards the game's Options letters (measured by
# font_shape.py): each letter widened to the game's width, the small letters'
# x-height, ascenders and descenders moved to the game's, its side bearings
# the game's where measured; then the weight that inks the game's label lines
# as darkly. Writes DualizeRecomp/resources/fonts/DualizeSans.ttf (OFL 1.1,
# a modified Kumbh Sans, renamed).
# python scripts/lettering/font_tune.py <KumbhSans variable .ttf> [weight]
import io, sys, json, unicodedata
import numpy as np
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer
from fontTools import subset
from fontTools.pens.recordingPen import DecomposingRecordingPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.pens.transformPen import TransformPen
from glyph_compose import ROOT, WORK

CAP = 14
shape = json.load(open(WORK / 'font_shape.json'))
OUT = ROOT / 'DualizeRecomp' / 'resources' / 'fonts' / 'DualizeSans.ttf'
KEEP = list(range(0x20, 0x7F)) + list(range(0xA0, 0x100))  # Latin-1

def ratio(c, key):
    v = shape.get(c)
    return v[key[0]] / v[key[1]] if v else None

def median(cs, key):
    r = [ratio(c, key) for c in cs if c in shape]
    return float(np.median(r))

WIDTH = median([c for c in shape if shape[c]['face_w'] >= 5], ('game_w', 'face_w'))
XH = median('acemnorsuvwxz', ('game_h', 'face_h'))
ASC = median('bdhk', ('game_h', 'face_h'))
DESC = median('gpqy', ('game_h', 'face_h'))

def width_of(base):
    """The letter's widening: its own measure drawn halfway to the others'."""
    v = shape.get(base)
    if v is None or v['face_w'] < 5:
        return WIDTH
    return WIDTH + 0.5 * (v['game_w'] / v['face_w'] - WIDTH)

def build(weight, widths=None):
    f = TTFont(sys.argv[1])
    axes = {a.axisTag: a.defaultValue for a in f['fvar'].axes}
    axes['wght'] = weight
    f = instancer.instantiateVariableFont(f, axes)
    b = io.BytesIO(); f.save(b); f = TTFont(io.BytesIO(b.getvalue()))
    opt = subset.Options(); opt.name_IDs = ['*']; opt.layout_features = ['kern']; opt.notdef_outline = True
    s = subset.Subsetter(opt); s.populate(unicodes=KEEP); s.subset(f)
    upm = f['head'].unitsPerEm
    cap_h = f['OS/2'].sCapHeight; x_h = f['OS/2'].sxHeight
    texel = cap_h / CAP  # font units per game texel
    gs = f.getGlyphSet(); glyf = f['glyf']; hmtx = f['hmtx']
    cmap = f.getBestCmap()
    by_glyph = {}
    for u, name in cmap.items():
        by_glyph.setdefault(name, chr(u))
    asc_top = max(gs[cmap[ord(c)]].__class__ and f['glyf'][cmap[ord(c)]].yMax for c in 'bdhk')
    desc_bot = min(f['glyf'][cmap[ord(c)]].yMin for c in 'gpqy')
    def warp_y(y, small):
        if not small:
            return y
        if y >= x_h:  # x-height to ascender: x-height moved, the ascender's top to the game's
            top_new = asc_top * ASC
            return x_h * XH + (y - x_h) * (top_new - x_h * XH) / max(1, asc_top - x_h)
        if y >= 0:
            return y * XH
        return y * DESC
    for name in list(glyf.keys()):
        c = by_glyph.get(name)
        if c is None or name == '.notdef':
            continue
        base = unicodedata.normalize('NFD', c)[0]
        small = base.islower()
        if not (base.isalpha()):
            continue
        rec = DecomposingRecordingPen(gs); gs[name].draw(rec)
        g = glyf[name]
        g.recalcBounds(glyf) if hasattr(g, 'recalcBounds') else None
        adv, lsb = hmtx[name]
        x0 = g.xMin if g.numberOfContours else 0; x1 = g.xMax if g.numberOfContours else adv
        mid = (x0 + x1) / 2; k = width_of(base) * (widths or {}).get(base, 1.0)
        pen = TTGlyphPen(None)
        class Warp:
            def __init__(self, out): self.out = out
            def _p(self, pt): return (mid + (pt[0] - mid) * k, warp_y(pt[1], small))
            def moveTo(self, p): self.out.moveTo(self._p(p))
            def lineTo(self, p): self.out.lineTo(self._p(p))
            def qCurveTo(self, *ps): self.out.qCurveTo(*[self._p(p) if p else p for p in ps])
            def curveTo(self, *ps): self.out.curveTo(*[self._p(p) for p in ps])
            def closePath(self): self.out.closePath()
            def endPath(self): self.out.endPath()
            def addComponent(self, *a): raise RuntimeError('decomposed')
        rec.replay(Warp(pen))
        ng = pen.glyph(); ng.recalcBounds(glyf); glyf[name] = ng
        nx0, nx1 = (ng.xMin, ng.xMax) if ng.numberOfContours else (0, 0)
        v = shape.get(base)
        if v and v['game_l'] is not None:
            l, r = v['game_l'] * texel, v['game_r'] * texel
        else:
            l, r = (x0 - 0) * k, (adv - x1) * k
        shift = l - nx0
        if shift:
            for i, (px, py) in enumerate(ng.coordinates):
                ng.coordinates[i] = (px + shift, py)
            ng.recalcBounds(glyf)
        hmtx[name] = (max(1, int(round(l + (nx1 - nx0) + r))), int(round(ng.xMin)) if ng.numberOfContours else 0)
    f['OS/2'].sxHeight = int(round(x_h * XH))
    for rec in f['name'].names:
        try:
            t = rec.toUnicode()
        except Exception:
            continue
        if 'Kumbh' in t:
            rec.string = t.replace('Kumbh Sans', 'Dualize Sans').replace('KumbhSans', 'DualizeSans')
    if 'GPOS' in f:  # kerning measured for the old widths
        del f['GPOS']
    if 'kern' in f:
        del f['kern']
    b = io.BytesIO(); f.save(b)
    return b.getvalue()

if __name__ == '__main__':
    weight = float(sys.argv[2]) if len(sys.argv) > 2 else 350
    path = WORK / 'glyph_widths.json'  # font_widths.py's per-letter widths, when measured
    data = build(weight, json.load(open(path)) if path.exists() else None)
    open(OUT, 'wb').write(data)
    print('width', round(WIDTH, 3), 'x-height', round(XH, 3), 'ascender', round(ASC, 3), 'descender', round(DESC, 3), 'weight', weight)
