# Walks the Options graphics page: from a state (GFX_STATE, default opt_row1.sav),
# one pad press per step (comma list; '-' waits), a frame saved after each step
# to build/check/gfx_walk/NN_step.png. User dir: build/check/users/<GFX_USER>
# (default gfx, no interpolation; q = the user's own settings).
import os, sys, time, glob, shutil
import numpy as np
from PIL import Image
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..')).replace(os.sep, '/')
sys.path.insert(0, ROOT + '/scripts')
from automation import Game
d = ROOT + '/build/check/frames/gw'; out = ROOT + '/build/check/gfx_walk'
shutil.rmtree(d, ignore_errors=True); shutil.rmtree(out, ignore_errors=True); os.makedirs(d); os.makedirs(out)
os.environ['AURORA_FRAME_DUMP'] = d
os.environ['AURORA_FRAME_DUMP_EVERY'] = '10'
steps = sys.argv[1].split(',')
wait = float(os.environ.get('GFX_WAIT', '1.5'))
USER = os.environ.get('GFX_USER', 'gfx')
g = Game(ROOT)
try:
    g.start(state=ROOT + '/build/states/native/' + os.environ.get('GFX_STATE', 'opt_row1.sav'), log=d + '/log.txt',
            extra_args=['--user', ROOT + '/build/check/users/' + USER] + (['--no-interpolation'] if USER == 'gfx' else []), hidden=os.environ.get('GFX_SHOW') is None)
    time.sleep(5)
    for i, b in enumerate(steps):
        if b != '-':
            g.pad(frames=6, **{b: 1})
        time.sleep(wait)
        raws = sorted(glob.glob(d + '/*.raw'))
        if raws:
            n = os.path.getsize(raws[-1]) // 4; w = int(round((n * 16 / 9) ** 0.5))
            a = np.fromfile(raws[-1], np.uint8).reshape(-1, w, 4)[:, :, :3]
            Image.fromarray(a).resize((1280, a.shape[0] * 1280 // w)).save(f'{out}/{i:02d}_{b}.png')
            if os.environ.get('GFX_FULL'): Image.fromarray(a).save(f'{out}/{i:02d}_{b}_full.png')
        for r in raws[:-2]:
            try: os.remove(r)
            except OSError: pass
finally:
    g.stop()
