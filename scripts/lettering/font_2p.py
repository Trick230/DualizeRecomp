# The faces the 2P menus' pre-drawn text is redrawn with (nights_2p_text.cpp),
# chosen and fitted against the game's text masks: static instances of the
# variable fonts at the fitted axes, cut to Latin-1 and renamed (OFL 1.1,
# see each face's license file). Writes into
# DualizeRecomp/resources/fonts.
# python scripts/lettering/font_2p.py <Inter[opsz,wght].ttf> <Figtree[wght].ttf> <Tinos-Bold.ttf>
import io, sys
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer
from fontTools import subset
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

KEEP = list(range(0x20, 0x7F)) + list(range(0xA0, 0x100)) + [0x2019]  # Latin-1, right quote
FONTS = ROOT / 'DualizeRecomp' / 'resources' / 'fonts'

def build(path, axes, family, style):
    f = TTFont(path)
    if axes:
        f = instancer.instantiateVariableFont(f, axes)
        b = io.BytesIO(); f.save(b); f = TTFont(io.BytesIO(b.getvalue()))
    opt = subset.Options(); opt.name_IDs = ['*']; opt.layout_features = ['kern']; opt.notdef_outline = True
    s = subset.Subsetter(opt); s.populate(unicodes=KEEP); s.subset(f)
    # A modified version takes a new name (OFL); copyright and trademark notices stay.
    name = f['name']
    name.names = [r for r in name.names if r.nameID not in (1, 2, 3, 4, 6, 16, 17, 21, 22, 25)]
    full = family + ('' if style == 'Regular' else ' ' + style)
    for i, text in ((1, family), (2, style), (3, full), (4, full), (6, (family + '-' + style).replace(' ', ''))):
        name.setName(text, i, 3, 1, 0x409)
    b = io.BytesIO(); f.save(b)
    return b.getvalue()

if __name__ == '__main__':
    inter, figtree, tinos = sys.argv[1:4]
    out = {
        'Dualize2PGothic.ttf': build(inter, {'opsz': 20, 'wght': 400}, 'Dualize 2P Gothic', 'Regular'),
        'Dualize2PSans.ttf': build(figtree, {'wght': 450}, 'Dualize 2P Sans', 'Regular'),
        'Dualize2PSans-Medium.ttf': build(figtree, {'wght': 550}, 'Dualize 2P Sans Medium', 'Regular'),
        'Dualize2PSerif-Bold.ttf': build(tinos, None, 'Dualize 2P Serif', 'Bold'),
    }
    for name, data in out.items():
        open(FONTS / name, 'wb').write(data)
        print(name, len(data))
