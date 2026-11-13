# The face the pause window's labels are drawn with (nights_pause.cpp): Tinos
# Bold Italic, the free face closest to the game's labels there, cut to
# Latin-1 and renamed (OFL 1.1, see DualizePauseSerif-OFL.txt). Writes into
# DualizeRecomp/resources/fonts.
# python scripts/lettering/font_pause.py <Tinos-BoldItalic.ttf>
import sys
from font_2p import FONTS, build

if __name__ == '__main__':
    data = build(sys.argv[1], None, 'Dualize Pause Serif', 'Bold Italic')
    open(FONTS / 'DualizePauseSerif-BoldItalic.ttf', 'wb').write(data)
    print('DualizePauseSerif-BoldItalic.ttf', len(data))
