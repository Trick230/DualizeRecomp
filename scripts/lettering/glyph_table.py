# Writes the Options lettering's table (sdk/nights_lettering_table.inc): for
# each letter and size, how the runtime makes it from the game's own lines
# (a cut: texture, window, cell; or a construction from other letters), and
# its side bearings. Numbers only: the letters themselves are cut from the
# player's disc at run time. Mirrors glyph_compose.py's glyph().
# python scripts/lettering/glyph_table.py  (after glyph_cuts.py)
import unicodedata
from glyph_compose import *
from glyph_compose import ROOT, LABEL_SOURCES

OUT = ROOT / 'DualizeRecomp' / 'sdk' / 'nights_lettering_table.inc'
eu = [s for s in srcs if 8 <= s < LABEL_SOURCES]
EU_PRESS = round(float(np.median([press[s] for s in eu])), 3)
EU_TRACK = round(float(np.median([TRACK[s] for s in eu])) * 13, 3)
SIZES = [('LABEL', 14, 1.0), ('SMALL', 13, EU_PRESS)]
DONOR = {'\u0301': 'éáó', '\u0300': 'èàò', '\u0302': 'êâ', '\u0308': 'äöü'}

def recipe(c, cap, pr):
    g = pick(c, cap)
    if g is not None: return ('CUT', g)
    if c == 'O': return ('O_FROM_C',) if recipe('C', cap, pr) else None
    if c == 'q': return ('Q_FROM_P',) if pick('p', cap) else None
    if c == 'G': return ('G_FROM_C',) if all(recipe(k, cap, pr) for k in 'COl') else None
    if c == 'U': return ('U_FROM_U',) if all(recipe(k, cap, pr) for k in 'uE') else None
    g = pick(c, cap, pressed_ok=True)
    if g is not None: return ('CUT', g)
    if c == '\u00d7': return ('TIMES_FROM_X',) if all(recipe(k, cap, pr) for k in 'xE') else None
    d = unicodedata.normalize('NFD', c)
    if len(d) == 2 and c.islower():
        donor = next((k for k in DONOR.get(d[1], '') if pick(k, cap, True)), None)
        if donor and recipe(d[0], cap, pr) and recipe('o', cap, pr):
            return ('ACCENT_SMALL', d[0], donor)
        return None
    if len(d) == 2 and c.isupper():
        if recipe(d[0], cap, pr) and recipe(c.lower(), cap, pr) and recipe('o', cap, pr):
            return ('ACCENT_CAPITAL', d[0], c.lower())
    return None

need = set(c for g in table if g['box'] for c in g['ch'])
import re
src = open(ROOT / 'DualizeRecomp' / 'sdk' / 'nights_text.cpp', encoding='utf8').read()
for m in re.finditer(r'\{("[^"]*"),\s*"[^"]*",\s*("[^"]*"),\s*("[^"]*"),\s*("[^"]*"),\s*("[^"]*")\}', src):
    for t in m.groups():
        need |= set(t.strip('"'))
need |= set('OGUqx\u00d7ÀÂÄÇÈÉÊËÎÏÔÖÙÛÜÁÍÓÚÑÌÒàâäçèéêëîïôöùûüáíóúñìò')
need -= {' '}

used_sources = {}
rows = []
for size, cap, pr in SIZES:
    for c in sorted(need):
        r = recipe(c, cap, pr)
        if r is None: continue
        base = unicodedata.normalize('NFD', c)[0]
        key = base if base in LSB else c if c in LSB else 'C'
        lsb, rsb = LSB[key], RSB[key]
        if r[0] == 'CUT':
            g = r[1]
            s = used_sources.setdefault(g['source'], len(used_sources))
            rows.append((c, size, 'CUT', s, g['x0'], g['x1'], 0, 0, lsb, rsb))
        elif r[0] in ('ACCENT_SMALL', 'ACCENT_CAPITAL'):
            rows.append((c, size, r[0], 0, 0, 0, r[1], r[2], lsb, rsb))
        else:
            rows.append((c, size, r[0], 0, 0, 0, 0, 0, lsb, rsb))

def cchar(c):
    return f"U'\\u{ord(c):04X}'" if ord(c) > 0x7E or c in "'\\" else f"U'{c}'"

lines = ['// Measured on the game's own lines of the Options labels' face by',
         '// scripts/lettering (letter cells cut by alignment to the known text): where',
         '// each letter is cut, how the missing ones are made, side bearings. Numbers only.', '']
lines.append('constexpr LetteringSource SOURCES[] = {')
for s, i in sorted(used_sources.items(), key=lambda kv: kv[1]):
    g0 = by_source[s][0]
    tex = g0['tex']; name, image = tex.rsplit('_', 1)
    lines.append(f'    {{"files/csd/textures/{name}.tls", {int(image)}, {g0["top"]}, {"true" if s >= LABEL_SOURCES else "false"}, '
                 f'{g0["cap"]:.2f}f, {g0["baseline"]:.2f}f, {press[s]:.3f}f, {THIN[s]:.3f}f}},')
lines.append('};')
lines.append('constexpr LetteringGlyph GLYPHS[] = {')
for c, size, make, s, x0, x1, base, donor, lsb, rsb in rows:
    b = cchar(base) if base else "U'\\0'"
    d = cchar(donor) if donor else "U'\\0'"
    lines.append(f'    {{{cchar(c)}, LetteringSize::{size}, Make::{make}, {s}, {x0}, {x1}, {b}, {d}, {lsb:.4f}f, {rsb:.4f}f}},')
lines.append('};')
lines.append(f'constexpr float SPACE = {SPACE:.4f}f;  // per cap texel')
lines.append(f'constexpr float SMALL_PRESS = {EU_PRESS}f, SMALL_TRACKING = {EU_TRACK}f;')
open(OUT, 'w', encoding='utf8', newline='\n').write('\n'.join(lines) + '\n')
print('sources', len(used_sources), 'glyphs', len(rows), 'press', EU_PRESS, 'track', EU_TRACK)
print('none:', ''.join(sorted(c for c in need if recipe(c, 13, EU_PRESS) is None)).encode('unicode_escape'))
