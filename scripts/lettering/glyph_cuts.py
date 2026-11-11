# The Options labels' face (Avenir-like) cut letter by letter out of the game's
# own lines of it. Each known line (a 24-row window of a texture) is split
# into one cell per character, spaces too, by dynamic programming: cuts on
# low-ink columns, cell widths near a prior (Nunito Sans first, then the widths
# learned from the clean cuts) times the line's scale, plus its tracking.
# Output: glyph_cuts.json (one entry per character instance) for glyph_compose.py.
# python scripts/lettering/glyph_cuts.py [check.png]  (DUALIZE_GAME: the extracted game, default extracted/nights)
import sys, json
from pathlib import Path
import os
ROOT = Path(__file__).resolve().parents[2]
GAME = Path(os.environ.get('DUALIZE_GAME', ROOT / 'extracted' / 'nights')) / 'files'
WORK = ROOT / 'build' / 'lettering'
WORK.mkdir(parents=True, exist_ok=True)
import tpl
import numpy as np
from PIL import Image, ImageFont
from fontTools.ttLib import TTFont
# The alignment's first width guess: a similar free face.
FONT = str(ROOT / 'DualizeRecomp' / 'resources' / 'fonts' / 'NunitoSans-Light.ttf')
LABELS = {
    'us': ['Language Settings', 'Check Aspect Ratio', 'Show Controller Window', 'Enable Weather Link',
           'Register/Delete Friend', 'Your Friend Code', 'Enable Auto-Login', 'Show Call Chime'],
    'fr': ['Paramètres de langue', "Vérifier le format de l'image", 'Afficher la fenêtre des commandes',
           'Activer la liaison météo', 'Inscrire/Supprimer ami', 'Ton code ami', 'Activer la connexion automatique',
           'Afficher le carillon'],
    'sp': ['Config. idioma', 'Comprobar proporción de imagen', 'Mostrar ventana de controles',
           'Activar conexión meteorológica', 'Registrar/Borrar amigo', 'Tu clave de amigo',
           'Activar conexión automática', 'Mostrar campanada'],
    'ga': ['Spracheinstellungen', 'Seitenverhältnis überprüfen', 'Controller-Fenster anzeigen',
           'Wetterverbindung aktivieren', 'Freund registrieren/löschen', 'Dein Freundescode',
           'Automatisches Einloggen aktivieren', 'Klingelzeichen anzeigen'],
    'it': ['Impostazioni lingua', 'Controlla proporzioni immagini', 'Mostra finestra comandi',
           'Attiva collegamento clima', 'Registra/elimina amico', 'Il tuo codice amico',
           'Abilita autenticazione automatica', 'Mostra campanello richiamo'],
}
# (texture, top row of the 24-row window, text): the label lines, then other
# lines of the same face (their window from the ink band's top minus 5).
SOURCES = [(f'option_all{l}_06', 24 * k, t) for l, ts in LABELS.items() for k, t in enumerate(ts)]
SOURCES += [
    ('option_allus_15', 13 - 5, "Set your TV's aspect ratio so that all five Ideyas"),
    ('option_allus_15', 35 - 5, 'appear as identical circles.'),
    ('mode2Pselectus_10', 3 - 5, 'With a Nearby Friend'),
    ('mode2Pselectus_10', 43 - 5, 'With a Faraway Friend'),
    ('mode2Pselectus_10', 83 - 5, 'With a Random Player'),
    ('2p_continueus_05', 1 - 5, 'Continue with same Nightopia'),
    ('2p_continueus_05', 41 - 5, 'Quit'),
    ('option_allus_06', 214 - 5, 'English Français Español Deutsch'),
    ('option_allus_06', 234 - 5, 'Italiano'),
]
LABEL_SOURCES = 40  # the first ones: whole label lines, their window clean
TRACK0 = 1.5
# Cuts the alignment gets wrong (a kerned pair), set by hand from a column
# dump: (source, character index) -> cell.
OVERRIDES = {(40, 9): (134, 145), (40, 10): (145, 157), (40, 11): (157, 160), (40, 12): (160, 169),
             (22, 12): (90, 98), (22, 13): (98, 100), (22, 14): (100, 110)}

tt = TTFont(FONT)
F = ImageFont.truetype(FONT, size=14 * tt['head'].unitsPerEm / tt['OS/2'].sCapHeight)
LEARNED = {}
def prior(c):
    return LEARNED.get(c, F.getlength(c) + TRACK0)

_cache = {}
def window(tex, top):
    if tex not in _cache:
        name, image = tex.rsplit('_', 1)
        _cache[tex] = tpl.load(GAME / 'csd' / 'textures' / (name + '.tls'), int(image))[:, :, 0].astype(np.float32) / 255
    a = _cache[tex]
    out = np.zeros((24, a.shape[1]), np.float32)
    for r in range(24):
        if 0 <= top + r < a.shape[0]:
            out[r] = a[top + r]
    return out

def segment(line, text):
    ink = line.max(0)
    cols = np.where(ink > 0.12)[0]
    x0, x1 = int(cols[0]), int(cols[-1]) + 1
    exp = np.array([prior(c) for c in text])
    n, W, start = len(text), len(ink), x0 - 1
    best = None
    for s in np.arange(0.58, 1.12, 0.02):
        for t in np.arange(-1.0, 1.01, 0.25):
            e = exp * s + t
            if abs(e.sum() - (x1 - start)) > 8 + 0.06 * (x1 - start):
                continue
            INF = 1e18
            cost = np.full((n + 1, W + 2), INF); back = np.zeros((n + 1, W + 2), int)
            cost[0, start] = 0
            ce = np.concatenate([[0], np.cumsum(e)])
            for k in range(1, n + 1):
                tol = 7 + 0.06 * ce[k]; lo, hi = int(max(0, start + ce[k] - tol)), int(min(W + 1, start + ce[k] + tol))
                for p in range(lo, hi + 1):
                    cut = (ink[p] if p < W else 0) ** 2 * 40 if k < n else 0
                    qs = np.arange(max(0, p - int(e[k - 1] * 1.7) - 2), p)
                    if len(qs) == 0: continue
                    d = (p - qs - e[k - 1]) / max(1.5, e[k - 1] * 0.2)
                    c = cost[k - 1, qs] + d * d + cut
                    j = int(np.argmin(c))
                    cost[k, p], back[k, p] = c[j], qs[j]
            p = min(range(x1 - 1, min(W + 1, x1 + 4)), key=lambda p: cost[n, p])
            if cost[n, p] >= INF: continue
            if best is None or cost[n, p] < best[0]:
                cuts = [p]
                for k in range(n, 0, -1):
                    p = back[k, p]; cuts.append(p)
                best = (float(cost[n, cuts[0]]), float(s), float(t), cuts[::-1])
    return best

def run():
    table = []
    for si, (tex, top, text) in enumerate(SOURCES):
        line = window(tex, top)
        if si >= LABEL_SOURCES:
            line[:4] = 0  # the line above's descenders
        total, s, t, cuts = segment(line, text)
        ink = line.max(0)
        for k, c in enumerate(text):
            a, b = OVERRIDES.get((si, k), (int(cuts[k]), int(cuts[k + 1])))
            clean = max(ink[a] if a < len(ink) else 0, ink[b] if b < len(ink) else 0)
            table.append(dict(source=si, tex=tex, top=top, ch=c, x0=a, x1=b, scale=round(s, 2), track=t,
                              clean=round(float(clean), 2)))
        print(si, tex, round(s, 2), t, round(total, 1), text.encode('ascii', 'replace').decode())
    return table

if __name__ == '__main__':
    table = run()
    for it in range(2):
        acc = {}
        for g in table:
            if g['clean'] < 0.25 and g['tex'] == 'option_allus_06':
                acc.setdefault(g['ch'], []).append((g['x1'] - g['x0'] - g['track']) / g['scale'])
        LEARNED = {c: float(np.median(v)) for c, v in acc.items()}
        table = run()
    json.dump(table, open(WORK / 'glyph_cuts.json', 'w', encoding='utf8'), ensure_ascii=False, indent=0)
    if len(sys.argv) > 1:
        rows = []
        for si, (tex, top, text) in enumerate(SOURCES):
            line = window(tex, top)[:, :512]
            rgb = np.stack([line] * 3, -1)
            rgb = np.pad(rgb, ((0, 0), (0, 512 - rgb.shape[1]), (0, 0)))
            for g in table:
                if g['source'] == si and g['x0'] < 512:
                    rgb[:, g['x0'], 0] = 1
            rows.append(rgb)
        img = (np.vstack(rows) * 255).astype(np.uint8)
        Image.fromarray(img).resize((1024, img.shape[0] * 2), Image.NEAREST).save(sys.argv[1])
