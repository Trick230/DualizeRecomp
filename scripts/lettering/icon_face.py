# Writes DualizeRecomp/resources/fonts/DualizeIcons.ttf: Figtree (OFL 1.1) at
# the weight icon_fit.py fitted to the letters inside the message font's
# pad-button icons, cut to those letters and renamed.
# python scripts/lettering/icon_face.py <Figtree variable .ttf> [weight]
import sys
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer
from fontTools import subset
from glyph_compose import ROOT

LETTERS = 'AB12Zabxy'
NAME = 'Dualize Icons'

if __name__ == '__main__':
    weight = float(sys.argv[2]) if len(sys.argv) > 2 else 450
    f = TTFont(sys.argv[1])
    axes = {a.axisTag: a.defaultValue for a in f['fvar'].axes}
    axes['wght'] = weight
    f = instancer.instantiateVariableFont(f, axes)
    options = subset.Options()
    options.name_IDs = ['*']
    options.layout_features = []
    options.hinting = False
    sub = subset.Subsetter(options)
    sub.populate(text=LETTERS)
    sub.subset(f)
    name = f['name']
    for record in list(name.names):
        if record.nameID in (1, 4, 16):
            record.string = NAME
        elif record.nameID == 6:
            record.string = NAME.replace(' ', '')
        elif record.nameID == 3:
            record.string = NAME + ' %d' % weight
        elif record.nameID in (2, 17):
            record.string = 'Regular'
        elif record.nameID == 25:
            name.removeNames(nameID=25)
    out = ROOT / 'DualizeRecomp' / 'resources' / 'fonts' / 'DualizeIcons.ttf'
    f.save(out)
    print(out, out.stat().st_size, 'bytes')
