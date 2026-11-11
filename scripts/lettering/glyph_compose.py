# Composes our Options labels from the game's own letter cuts (glyph_cuts.json).
# Every instance is measured: its ink box (cell trimmed to ink), its line's
# cap height (rows of its capitals / x-height letters) and press (ink widths
# against the English labels' at the same cap). Two sizes, as the game sets
# them: English labels cap 14, the other languages' cap 13 (narrower).
# Side bearings: least squares over neighbouring pairs of clean lines.
# A missing O is the game's C mirrored about its middle (a geometric face).
# python scripts/lettering/glyph_compose.py out.png  (after glyph_cuts.py)
import sys, json, collections
import numpy as np
from PIL import Image
from glyph_cuts import *
from glyph_cuts import LABEL_SOURCES, SOURCES, window, F, ROOT, WORK
table = json.load(open(WORK / 'glyph_cuts.json', encoding='utf8'))
T = 0.12
from scipy import ndimage
_labels = {}
def components(key, line):
    if key not in _labels:
        lab, n = ndimage.label(line > 0.25, structure=np.ones((3, 3)))
        _labels[key] = lab
    return _labels[key]
for g in table:
    line = window(g['tex'], g['top'])
    if g['source'] >= LABEL_SOURCES:
        line[:4] = 0
    g['line'] = line
    g['box'] = None
    if g['ch'] == ' ':
        continue
    # The letter: its cell's columns, less the ink of components centred
    # outside it (a neighbour reaching in), plus its own components' ink
    # reaching out of it (a kerned overhang). Components spanning several
    # letters keep to the cell.
    lab = components((g['tex'], g['top']), line)
    x0, x1 = g['x0'], g['x1']
    keep = np.zeros(line.shape, bool)
    keep[:, x0:x1] = True
    lo, hi = max(0, x0 - 8), min(line.shape[1], x1 + 8)
    for k in np.unique(lab[:, lo:hi]):
        if k == 0: continue
        comp = lab == k
        ys, xs = np.nonzero(comp)
        wts = line[ys, xs]
        cx = float((xs * wts).sum() / wts.sum())
        span = xs.max() - xs.min() + 1
        if span > (x1 - x0) * 1.4 + 3:
            continue
        inside = x0 <= cx < x1
        if inside:
            keep |= comp
        else:
            keep &= ~ndimage.binary_dilation(comp, structure=np.ones((3, 3)))
    img = np.where(keep, line, 0)
    cols = np.where(img.max(0) > T)[0]
    if len(cols) == 0:
        continue
    a, b = int(cols[0]), int(cols[-1]) + 1
    g['box'] = (a, b)
    g['img'] = img[:, a:b]
    rows = np.where(g['img'].max(1) > 0.3)[0]
    if len(rows) == 0:
        rows = np.where(g['img'].max(1) > T)[0]
    g['rows'] = (int(rows[0]), int(rows[-1]) + 1)

# A line's cap: its flat capitals' heights, else its x-height letters' times
# the face's ratio.
CAPS, XS = set('ABDEFHIKLMNPRTVWXZ'), set('acemnorsuvwxz')
by_source = collections.defaultdict(list)
for g in table:
    by_source[g['source']].append(g)
xratio = []
for s, gs in by_source.items():
    caps = [g['rows'][1] - g['rows'][0] for g in gs if g['box'] and g['ch'] in CAPS]
    xs = [g['rows'][1] - g['rows'][0] for g in gs if g['box'] and g['ch'] in XS]
    if caps and xs:
        xratio.append(np.median(caps) / np.median(xs))
XR = float(np.median(xratio))
cap_of = {}
for s, gs in by_source.items():
    caps = [g['rows'][1] - g['rows'][0] for g in gs if g['box'] and g['ch'] in CAPS]
    xs = [g['rows'][1] - g['rows'][0] for g in gs if g['box'] and g['ch'] in XS]
    cap_of[s] = float(np.median(caps)) if caps else float(np.median(xs)) * XR
    base = [g['rows'][1] for g in gs if g['box'] and (g['ch'] in CAPS or g['ch'] in XS)]
    for g in gs:
        g['cap'] = cap_of[s]; g['baseline'] = float(np.median(base))

# Stroke weight: other screens' lines are drawn heavier than the labels;
# thin their letters (coverage (c - k) / (1 - k)) to the labels' ink.
_ref = collections.defaultdict(list)
for g in table:
    if g['box'] and g['source'] < LABEL_SOURCES and g['clean'] < 0.3:
        _ref[g['ch']].append(g['img'].sum() / g['cap'] ** 2)
_ref = {c: np.median(v) for c, v in _ref.items()}
def _weight(s, k):
    r = [np.clip((g['img'] - k) / (1 - k), 0, 1).sum() / g['cap'] ** 2 / _ref[g['ch']]
         for g in by_source[s] if g['box'] and g['ch'] in _ref and g['clean'] < 0.3]
    return float(np.median(r)) if r else 1.0
THIN = {}
for s in by_source:
    k = 0.0
    if s >= LABEL_SOURCES and _weight(s, 0) > 1.02:
        lo, hi = 0.0, 0.8
        for _ in range(20):
            m = (lo + hi) / 2
            lo, hi = (m, hi) if _weight(s, m) > 1 else (lo, m)
        k = round(lo, 3)
    THIN[s] = k
    if k:
        for g in by_source[s]:
            if g['box']:
                g['img'] = np.clip((g['img'] - k) / (1 - k), 0, 1)

# Ink widths per cap texel against the English labels: a line's press.
ref = collections.defaultdict(list)
for g in table:
    if g['source'] < 8 and g['box']:
        ref[g['ch']].append((g['box'][1] - g['box'][0]) / g['cap'])
ref = {c: np.median(v) for c, v in ref.items()}
press = {}
for s, gs in by_source.items():
    r = [((g['box'][1] - g['box'][0]) / g['cap']) / ref[g['ch']] for g in gs if g['box'] and g['ch'] in ref and ref[g['ch']] * g['cap'] > 4]
    press[s] = float(np.median(r)) if len(r) >= 3 else 1.0
for g in table:
    g['press'] = press[g['source']]
print('cap', {s: round(c, 1) for s, c in cap_of.items()})
print('press', {s: round(p, 2) for s, p in press.items()})

# Bearings per cap texel: gap(a,b)/cap = rsb[a] + lsb[b] + track[line] (+ space).
chars = sorted({g['ch'] for g in table if g['box']})
ci = {c: i for i, c in enumerate(chars)}
srcs = sorted(by_source)
n = 2 * len(chars) + len(srcs) + 1
A, y = [], []
for s in srcs:
    gs = by_source[s]
    for i in range(len(gs) - 1):
        a = gs[i]
        if not a['box'] or a['clean'] > 0.3: continue
        j = i + 1
        while j < len(gs) and gs[j]['ch'] == ' ': j += 1
        if j >= len(gs) or j - i > 2 or not gs[j]['box'] or gs[j]['clean'] > 0.3: continue
        b = gs[j]
        if a['ch'] == 'f' and b['ch'] in 'fil' and j == i + 1:
            continue  # the game's fi/fl/ff ligatures
        row = np.zeros(n)
        row[2 * ci[a['ch']] + 1] += 1; row[2 * ci[b['ch']]] += 1; row[2 * len(chars) + srcs.index(s)] = 1
        row[-1] = j - i - 1
        A.append(row); y.append((b['box'][0] - a['box'][1]) / a['cap'] / a['press'])
A, y = np.array(A), np.array(y)
# Prior: the similar face's bearings (per cap), shifted by the game's mean
# difference on letters seen often; it decides for letters seen in few pairs.
def nunito_bearings(c):
    m = F.getmask(c).getbbox()
    if not m:
        return 0.06, 0.06
    x0 = F.getbbox(c)[0] + m[0] * 0  # bbox already in pen space
    b = F.getbbox(c)
    return b[0] / 14, (F.getlength(c) - b[2]) / 14
A0, y0 = A, y
sol0, *_ = np.linalg.lstsq(np.vstack([A0, np.eye(n) * 0.5]), np.concatenate([y0, np.full(n, 0.06)]), rcond=None)
counts = (A0[:, :2 * len(chars)] != 0).sum(0)
diff = [sol0[2 * i + k] - nunito_bearings(c)[k] for c, i in ci.items() for k in (0, 1) if counts[2 * i + k] >= 10]
SHIFT = float(np.median(diff))
reg = [(np.eye(n)[2 * i + k] * 1.0, nunito_bearings(c)[k] + SHIFT) for c, i in ci.items() for k in (0, 1)]
reg += [(np.eye(n)[2 * len(chars) + srcs.index(s)] * 3, 0) for s in srcs if s < 8]
A2 = np.vstack([A] + [r for r, _ in reg]); y2 = np.concatenate([y, [v for _, v in reg]])
sol, *_ = np.linalg.lstsq(A2, y2, rcond=None)
for _ in range(3):  # drop pairs a bad cut threw off, refit
    ok = np.abs(A @ sol - y) * 14 < 2.5
    A2 = np.vstack([A[ok]] + [r for r, _ in reg]); y2 = np.concatenate([y[ok], [v for _, v in reg]])
    sol, *_ = np.linalg.lstsq(A2, y2, rcond=None)
A, y = A[ok], y[ok]
LSB = {c: float(sol[2 * i]) for c, i in ci.items()}
RSB = {c: float(sol[2 * i + 1]) for c, i in ci.items()}
SPACE = float(sol[-1])
TRACK = {s: float(sol[2 * len(chars) + k]) for k, s in enumerate(srcs)}
res = A @ sol - y
print('pairs', len(y), 'rms', round(float(np.sqrt((res ** 2).mean())) * 14, 2), 'texels; space', round(SPACE * 14, 2))
print('track', {s: round(t * 14, 2) for s, t in TRACK.items()})

def instances(c):
    gs = [g for g in table if g['ch'] == c and g['box'] and g['press'] > 0.84]
    # Ink width per cap near the similar face's (Nunito) letter: a wrong cut is far off.
    nb = F.getmask(c).getbbox()
    nw = (nb[2] - nb[0]) / 14 if nb else 0
    if nw > 0.15:
        gs = [g for g in gs if 0.6 < (g['box'][1] - g['box'][0]) / g['cap'] / g['press'] / nw < 1.5]
    # Drop cuts whose ink width (per cap, unpressed) strays from the letter's median: a neighbour got in.
    if len(gs) >= 3:
        w = np.array([(g['box'][1] - g['box'][0]) / g['cap'] / g['press'] for g in gs])
        m = np.median(w)
        gs = [g for g, x in zip(gs, w) if abs(x - m) <= max(0.12 * m, 1.2 / 14)]
    return gs

def pick(c, cap, pressed_ok=False):
    best = None
    pool = instances(c)
    if not pool and pressed_ok:
        pool = [g for g in table if g['ch'] == c and g['box']]
    for g in pool:
        score = g['clean'] * 3 + abs(1 - g['press']) * 3 + abs(np.log(cap / g['cap'])) * 4 + (0 if g['source'] < LABEL_SOURCES else 1.5)
        if best is None or score < best[0]:
            best = (score, g)
    return best[1] if best else None

def resample(im, w, h):
    return np.asarray(Image.fromarray((np.clip(im, 0, 1) * 255).astype(np.uint8)).resize((w, h), Image.BICUBIC), np.float32) / 255

import unicodedata
def fit(g, im, cap, press):
    """im (a cut of instance g) scaled to cap and press, its baseline on row 19."""
    k = cap / g['cap']
    sx = k * press / g['press']
    if abs(k - 1) > 0.02 or abs(sx - 1) > 0.02:
        h = int(round(24 * k)); w = max(1, int(round(im.shape[1] * sx)))
        big = resample(im, w, h)
        base = int(round(g['baseline'] * k))
        out = np.zeros((24, w), np.float32)
        for r in range(24):
            sr = r - 19 + base
            if 0 <= sr < h: out[r] = big[sr]
        return out
    shift = int(round(19 - g['baseline']))
    out = np.zeros((24, im.shape[1]), np.float32)
    for r in range(24):
        if 0 <= r - shift < 24: out[r] = im[r - shift]
    return out

def glyph(c, cap, press):
    """Ink image of c at cap, its baseline on row 19 of 24."""
    g = pick(c, cap)
    if g is not None:
        return fit(g, g['img'], cap, press)
    if c == 'O':  # the C's left half and its mirror
        g = pick('C', cap)
        if g is None: return None
        im = g['img']; w = im.shape[1]
        half = im[:, :(w + 1) // 2]
        return fit(g, np.concatenate([half, half[:, ::-1][:, w % 2:]], 1), cap, press)
    if c == 'q':  # the p mirrored
        g = pick('p', cap)
        return fit(g, g['img'][:, ::-1], cap, press) if g else None
    if c == 'G':  # the C, the O's lower right quarter up to the middle, a bar in from there
        Cc, O = glyph('C', cap, press), glyph('O', cap, press)
        l = glyph('l', cap, press)
        if Cc is None or O is None or l is None: return None
        w = max(Cc.shape[1], O.shape[1])
        out = np.zeros((24, w), np.float32)
        out[:, :Cc.shape[1]] = Cc
        rows = np.where(O.max(1) > 0.3)[0]; top, bottom = rows[0], rows[-1] + 1
        mid = (top + bottom) // 2
        quarter = O.copy(); quarter[:mid] = 0; quarter[:, :O.shape[1] // 2] = 0
        out[:, :O.shape[1]] = np.maximum(out[:, :O.shape[1]], quarter)
        stem = l[:, np.where(l.max(0) > T)[0]]  # a stroke's cross-section, turned for the bar
        prof = stem[top + 3:top + 4].ravel()  # one row across the l's stem
        right = int(np.where(O[mid + 1] > 0.3)[0][-1]) + 1
        left = int(round(O.shape[1] * 0.52))
        for i, v in enumerate(prof):
            r = mid - len(prof) // 2 + i
            out[r, left:right] = np.maximum(out[r, left:right], v)
        return out
    if c == 'U':  # the u made even (its left half and the mirror), its stems drawn up to the capitals' height
        u, H = glyph('u', cap, press), glyph('E', cap, press)
        if u is None or H is None: return None
        w = u.shape[1]
        half = u[:, :(w + 1) // 2]
        im = np.concatenate([half, half[:, ::-1][:, w % 2:]], 1)
        utop = int(np.where(im.max(1) > 0.3)[0][0]); htop = int(np.where(H.max(1) > 0.3)[0][0])
        stem_row = im[utop + 2]
        out = im.copy()
        for r in range(htop, utop + 2):
            out[r] = stem_row
        return out
    g = pick(c, cap, pressed_ok=True)
    if g is not None:
        return fit(g, g['img'], cap, press)
    if c == '×':  # the x, centred on the capitals' middle
        x, E = glyph('x', cap, press), glyph('E', cap, press)
        if x is None or E is None: return None
        xr = np.where(x.max(1) > 0.3)[0]; er = np.where(E.max(1) > 0.3)[0]
        shift = int(round((xr[0] + xr[-1]) / 2 - (er[0] + er[-1]) / 2))
        out = np.zeros_like(x)
        for r in range(24):
            if 0 <= r + shift < 24: out[r] = x[r + shift]
        return out
    d = unicodedata.normalize('NFD', c)
    if len(d) == 2 and c.islower():  # a small letter and another's accent (an i loses its dot)
        DONOR = {'́': 'éáó', '̀': 'èàò', '̂': 'êâ', '̈': 'äöü'}
        donor = next((glyph(k, cap, press) for k in DONOR.get(d[1], '') if pick(k, cap, True)), None)
        base, o = glyph(d[0], cap, press), glyph('o', cap, press)
        if donor is None or base is None or o is None: return None
        xtop = int(np.where(o.max(1) > 0.3)[0][0])
        out = base.copy()
        if d[0] in 'ij':
            out[:xtop - 1] = 0
        acc = donor[:xtop - 1]
        ac = np.where(acc.max(0) > T)[0]
        acc = acc[:, ac[0]:ac[-1] + 1]
        bc = np.where(out.max(0) > T)[0]
        cx = (bc[0] + bc[-1] + 1) / 2
        x0 = int(round(cx - acc.shape[1] / 2))
        if x0 < 0:
            out = np.pad(out, ((0, 0), (-x0, 0))); x0 = 0
        if x0 + acc.shape[1] > out.shape[1]:
            out = np.pad(out, ((0, 0), (0, x0 + acc.shape[1] - out.shape[1])))
        out[:xtop - 1, x0:x0 + acc.shape[1]] = np.maximum(out[:xtop - 1, x0:x0 + acc.shape[1]], acc)
        return out
    if len(d) == 2 and c.isupper():  # an accented capital: the capital and the small letter's accent, raised
        base = glyph(d[0], cap, press)
        small = glyph(c.lower(), cap, press)
        x = glyph('o', cap, press)
        if base is None or small is None or x is None: return None
        xtop = int(np.where(x.max(1) > 0.3)[0][0])
        acc = small[:xtop - 1].copy()
        ac = np.where(acc.max(0) > T)[0]
        if len(ac) == 0: return base
        acc = acc[:, ac[0]:ac[-1] + 1]
        btop = int(np.where(base.max(1) > 0.3)[0][0])
        lift = xtop - btop
        out = base.copy()
        x0 = (base.shape[1] - acc.shape[1]) // 2
        for r in range(acc.shape[0]):
            rr = r - lift
            if 0 <= rr < 24:
                seg = out[rr, max(0, x0):x0 + acc.shape[1]]
                out[rr, max(0, x0):x0 + acc.shape[1]] = np.maximum(seg, acc[r, max(0, -x0):max(0, -x0) + len(seg)])
        return out
    return None

def compose(text, cap, press=1.0, track=0.0, x=4.0):
    out = np.zeros((24, 256), np.float32)
    pen = x
    missing = []
    for i, c in enumerate(text):
        if c == ' ':
            pen += SPACE * cap * press; continue
        im = glyph(c, cap, press)
        if im is None:
            missing.append(c); pen += 0.5 * cap; continue
        base = unicodedata.normalize('NFD', c)[0]
        key = base if base in LSB else c if c in LSB else 'C'
        px = int(round(pen + LSB[key] * cap * press))
        w = min(im.shape[1], 256 - px)
        if w > 0:
            out[:, px:px + w] = np.maximum(out[:, px:px + w], im[:, :w])
        pen += (LSB[key] + RSB[key]) * cap * press + im.shape[1] + track
    return out, missing

if __name__ == '__main__':
    EN = ['Language Settings', 'Subtitles', 'Graphics Settings', 'Rumble', 'Weather Link', 'Fullscreen', 'Window Size',
          'Resolution Scale', 'V-Sync', 'Frame Rate', 'Anti-Aliasing', 'Transparency Anti-Aliasing', 'Ambient Occlusion']
    FR = ['Paramètres de langue', 'Sous-titres', 'Paramètres graphiques', 'Vibrations', 'Liaison météo', 'Plein écran',
          'Taille de la fenêtre', 'Échelle de résolution', 'Fréquence d\'images', 'Anticrénelage', 'Occlusion ambiante']
    DE = ['Spracheinstellungen', 'Untertitel', 'Grafikeinstellungen', 'Vibration', 'Wetterverbindung', 'Vollbild',
          'Fenstergröße', 'Auflösungsskalierung', 'Bildrate', 'Kantenglättung', 'Umgebungsverdeckung']
    rows = [window('option_allus_06', 0), np.full((2, 256), 0.6)]
    allmiss = set()
    for t in EN:
        o, m = compose(t, 14); allmiss |= set(m)
        rows += [o, np.full((1, 256), 0.3)]
    eu = [s for s in srcs if 8 <= s < LABEL_SOURCES]
    eu_press = float(np.median([press[s] for s in eu])); eu_track = float(np.median([TRACK[s] for s in eu])) * 13
    print('EU press', round(eu_press, 2), 'track', round(eu_track, 2))
    rows += [np.full((3, 256), 0.6), window('option_allfr_06', 0), np.full((2, 256), 0.6)]
    for t in FR + DE:
        o, m = compose(t, 13, press=eu_press, track=eu_track); allmiss |= set(m)
        rows += [o, np.full((1, 256), 0.3)]
    print('missing', ''.join(sorted(allmiss)).encode('unicode_escape'))
    img = np.vstack(rows)
    Image.fromarray((img * 255).astype(np.uint8)).resize((1024, img.shape[0] * 3), Image.NEAREST).save(sys.argv[1])

def coverage():
    """Letters our Options texts need that neither size has, after the constructions."""
    import re
    src = open(ROOT / 'DualizeRecomp' / 'sdk' / 'nights_text.cpp', encoding='utf8').read()
    labels = []
    for m in re.finditer(r'\{("[^"]*"),\s*"[^"]*",\s*("[^"]*"),\s*("[^"]*"),\s*("[^"]*"),\s*("[^"]*")\}', src):
        texts = [g.strip('"') for g in m.groups()]
        if len(texts[0]) <= 30 and not texts[0][0].islower():
            labels.append(texts)
    values = ['ON', 'OFF', 'MAX', 'MÁX', '1920×1080', '1.5x', '2x', '4x', '8x', '30', '60', '120', '144', '165', '240', '0.5x']
    need = set(''.join(sum(labels, []) + values)) - {' '}
    miss = {c for c in need if glyph(c, 14, 1.0) is None or glyph(c, 13, 0.9) is None}
    return sorted(miss)
