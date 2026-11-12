"""Fixed performance suite: uncapped scenes from savestates, builds run
interleaved, results appended to build/perf/history.csv.

Usage (from the repo root):
  python scripts/check/perf_suite.py [--rounds N] [--secs S] [--scenes a,b] [label=exe ...]
With no label, the current release build runs as `head`. Each round runs every
scene on every build in turn (interleaved), so drift (clocks, the GPU's power
states) hits all builds alike. Prints the median per build and scene.
"""
import argparse, csv, datetime, os, re, statistics, subprocess, sys, time

ROOT = os.getcwd()
sys.path.insert(0, os.path.join(ROOT, 'scripts'))
from automation import Game

STATES = os.path.join(ROOT, 'build', 'states', 'native')
OUT = os.path.join(ROOT, 'build', 'perf')
USER = os.path.join(OUT, 'user')  # default settings, the same for every run
SKIP_SPEED, SKIP_GX = 1, 2  # the first stats lines cover the state load
# Scene -> (savestate, extra arguments). forest_gpu loads the GPU (3x render
# scale, ambient occlusion, MSAA 8x by default); menu is the Options screen.
SCENES = {'forest': ('forest', []), 'gate': ('gate', []), 'bluetrees': ('bluetrees', []),
          'trees': ('trees', []), 'dark1': ('dark1', []),
          'forest_gpu': ('forest', ['--render-scale', '3', '--ao']), 'menu': ('options', [])}


def run(exe, scene, secs, log):
    state, extra = SCENES[scene]
    game = Game(ROOT, exe=exe)
    env = dict(os.environ)
    os.environ['DUALIZE_GX_STATS'] = '1'
    try:
        game.start(state=os.path.join(STATES, state + '.sav'), log=log,
                   extra_args=['--user', USER, '--unthrottled', '--no-vsync', '--no-interpolation', *extra])
        time.sleep(secs)
    finally:
        game.stop()
        os.environ.clear()
        os.environ.update(env)
    text = open(log, errors='replace').read()
    speed = [float(m) for m in re.findall(r'speed (\d+(?:\.\d+)?)%', text)][SKIP_SPEED:]
    gx = re.findall(r'\[gx\] (\d+) fps.*?frame ms ([\d.]+) sd ([\d.]+) max ([\d.]+)(?:, gpu ms ([\d.]+))?', text)[SKIP_GX:]
    if not speed or not gx:
        return None
    return {'speed': statistics.median(speed), 'fps': statistics.median(int(g[0]) for g in gx),
            'frame_ms': statistics.median(float(g[1]) for g in gx),
            'frame_sd_ms': statistics.median(float(g[2]) for g in gx),
            'max_frame_ms': max(float(g[3]) for g in gx), 'samples': len(gx),
            'gpu_ms': statistics.median(float(g[4] or 0) for g in gx)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rounds', type=int, default=2)
    ap.add_argument('--secs', type=float, default=25)
    ap.add_argument('--scenes', default='forest,gate,forest_gpu,menu', help=','.join(SCENES))
    ap.add_argument('--no-history', action='store_true', help="don't append to history.csv (training runs)")
    ap.add_argument('builds', nargs='*', help='label=path/to/DualizeRecomp.exe')
    a = ap.parse_args()
    builds = [b.split('=', 1) for b in a.builds] or [['head', None]]
    commit = subprocess.run(['git', 'rev-parse', '--short', 'HEAD'], capture_output=True, text=True).stdout.strip()
    os.makedirs(USER, exist_ok=True)
    history = os.path.join(OUT, 'history.csv' if not a.no_history else 'scratch.csv')
    new = not os.path.exists(history)
    results = {}
    with open(history, 'a', newline='') as f:
        w = csv.writer(f)
        if new:
            w.writerow(['date', 'commit', 'build', 'scene', 'round', 'speed_pct', 'fps', 'frame_ms', 'frame_sd_ms', 'max_frame_ms', 'samples', 'gpu_ms'])
        for r in range(a.rounds):
            for scene in a.scenes.split(','):
                for label, exe in builds:
                    m = run(exe, scene, a.secs, os.path.join(OUT, f'{label}_{scene}_{r}.log'))
                    if m is None:
                        print(f'{label} {scene} round {r}: no stats (see build/perf/{label}_{scene}_{r}.log)', flush=True)
                        continue
                    results.setdefault((label, scene), []).append(m)
                    w.writerow([datetime.datetime.now().isoformat(timespec='seconds'), commit, label, scene, r,
                                f"{m['speed']:.0f}", m['fps'], f"{m['frame_ms']:.3f}", f"{m['frame_sd_ms']:.3f}", f"{m['max_frame_ms']:.2f}", m['samples'], f"{m['gpu_ms']:.2f}"])
                    f.flush()
    for (label, scene), ms in sorted(results.items()):
        med = lambda k: statistics.median(x[k] for x in ms)
        print(f'{label:>10} {scene:>8}: speed {med("speed"):.0f}%  fps {med("fps"):.0f}  frame {med("frame_ms"):.3f} ms'
              f' (sd {med("frame_sd_ms"):.2f})  gpu {med("gpu_ms"):.2f} ms  max frame {max(x["max_frame_ms"] for x in ms):.2f} ms  ({len(ms)} runs)')


if __name__ == '__main__':
    main()
