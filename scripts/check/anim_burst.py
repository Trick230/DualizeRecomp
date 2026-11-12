# Pads a walk (comma list as gfx_walk), then on the last step keeps every frame
# dumped (every 2nd frame) for BURST seconds: build/check/anim_burst/NNN.png.
import glob, os, shutil, sys, time
import numpy as np
from PIL import Image
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..')).replace(os.sep, '/')
sys.path.insert(0, ROOT + '/scripts')
from automation import Game
d = ROOT + '/build/check/frames/ab'; out = ROOT + '/build/check/anim_burst'
shutil.rmtree(d, ignore_errors=True); shutil.rmtree(out, ignore_errors=True); os.makedirs(d); os.makedirs(out)
os.environ['AURORA_FRAME_DUMP'] = d
os.environ['AURORA_FRAME_DUMP_EVERY'] = '2'
steps = sys.argv[1].split(',')
burst = float(os.environ.get('BURST', '1.5'))
g = Game(ROOT)
try:
    g.start(state=ROOT + '/build/states/native/' + os.environ.get('GFX_STATE', 'opt_row1.sav'), log=d + '/log.txt',
            extra_args=['--user', ROOT + '/build/check/users/gfx', '--no-interpolation'])
    time.sleep(5)
    for b in steps[:-1]:
        if b != '-':
            g.pad(frames=6, **{b: 1})
        time.sleep(1.5)
    for r in glob.glob(d + '/*.raw'):
        try: os.remove(r)
        except OSError: pass
    g.pad(frames=6, **{steps[-1]: 1})
    time.sleep(burst)
finally:
    g.stop()
for i, r in enumerate(sorted(glob.glob(d + '/*.raw'))):
    n = os.path.getsize(r) // 4; w = int(round((n * 16 / 9) ** 0.5))
    a = np.fromfile(r, np.uint8).reshape(-1, w, 4)[:, :, :3]
    Image.fromarray(a).save(f'{out}/{i:03d}.png')
print(len(os.listdir(out)), 'frames')
