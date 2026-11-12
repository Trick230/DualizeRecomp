# Runs a cutscene state for a while (log to build/check/sub_<state>.log) and
# saves a frame every 2 s to build/check/sub_<state>/NN.png.
import glob, os, shutil, sys, time
import numpy as np
from PIL import Image
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..')).replace(os.sep, '/')
sys.path.insert(0, ROOT + '/scripts')
from automation import Game
state = sys.argv[1]
seconds = int(sys.argv[2]) if len(sys.argv) > 2 else 16
d = ROOT + '/build/check/frames/sp'; out = ROOT + f'/build/check/sub_{state}'
shutil.rmtree(d, ignore_errors=True); shutil.rmtree(out, ignore_errors=True); os.makedirs(d); os.makedirs(out)
os.environ['AURORA_FRAME_DUMP'] = d
os.environ['AURORA_FRAME_DUMP_EVERY'] = '10'
g = Game(ROOT)
try:
    g.start(state=ROOT + f'/build/states/native/{state}.sav', log=ROOT + f'/build/check/sub_{state}.log',
            extra_args=['--user', ROOT + '/build/check/users/gfx', '--no-interpolation'])
    for i in range(seconds // 2):
        time.sleep(2)
        raws = sorted(glob.glob(d + '/*.raw'))
        if raws:
            n = os.path.getsize(raws[-1]) // 4; w = int(round((n * 16 / 9) ** 0.5))
            a = np.fromfile(raws[-1], np.uint8).reshape(-1, w, 4)[:, :, :3]
            Image.fromarray(a).resize((640, a.shape[0] * 640 // w)).save(f'{out}/{i:02d}.png')
        for r in raws[:-2]:
            try: os.remove(r)
            except OSError: pass
finally:
    g.stop()
