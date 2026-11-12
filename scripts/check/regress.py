"""Regression check: replays fixed input movies from savestates and compares
the guest RAM hashes with references, so a change that alters what the game
computes shows up without looking at a single frame.

Usage (from the repo root, after a build):
  python scripts/check/regress.py [--update] [--scenes a,b] [--exe path]
--update records the current build's hashes as the references (do it only
from a build known to be good). Exit status 1 when a scene differs, misses
its movie's end or runs interpreted code it did not before.

Inputs live in build/regress (derived from the game, never committed):
movies/<scene>.movie, user_template/ (the NAND each run starts from, copied
fresh so saves made by one run don't reach the next), ref/<scene>.txt.
Logs go to build/regress/out.
"""
import argparse, os, re, shutil, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..')).replace(os.sep, '/')
sys.path.insert(0, os.path.join(ROOT, 'scripts'))
from automation import Game

BASE = os.path.join(ROOT, 'build', 'regress')
STATES = os.path.join(ROOT, 'build', 'states', 'native')
SCENES = {'forest': 'forest.sav', 'gate': 'gate.sav', 'bluetrees': 'bluetrees.sav',
          'dark1': 'dark1.sav', 'cutscene': 'cutscene_eyes.sav', 'boot': None}
TIMEOUT = 300


def run(scene, exe):
    out = os.path.join(BASE, 'out')
    user = os.path.join(out, 'user_' + scene)
    shutil.rmtree(user, ignore_errors=True)
    shutil.copytree(os.path.join(BASE, 'user_template'), user)
    log = os.path.join(out, scene + '.log')
    game = Game(ROOT, exe=exe)
    state = SCENES[scene]
    try:
        game.start(state=os.path.join(STATES, state) if state else None, log=log,
                   extra_args=['--user', user, '--unthrottled', '--no-vsync', '--no-interpolation',
                               '--hash-every', '10', '--replay-input', os.path.join(BASE, 'movies', scene + '.movie')])
        deadline = time.time() + TIMEOUT
        while game.proc.poll() is None and time.time() < deadline:
            time.sleep(0.25)
    finally:
        game.stop()
    text = open(log, errors='replace').read()
    hashes = [l[len('[hash] '):].strip() for l in text.splitlines() if l.startswith('[hash] ')]
    ended = '[movie] played to its end' in text
    interp = (re.findall(r'interpreted (\d+)', text) or ['?'])[-1]
    return hashes, ended, interp


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--update', action='store_true', help='record the references from this build')
    ap.add_argument('--scenes', default=','.join(SCENES))
    ap.add_argument('--exe', default=None, help='game executable (default: the release build)')
    a = ap.parse_args()
    os.makedirs(os.path.join(BASE, 'out'), exist_ok=True)
    os.makedirs(os.path.join(BASE, 'ref'), exist_ok=True)
    failed = []
    for scene in a.scenes.split(','):
        t0 = time.time()
        hashes, ended, interp = run(scene, a.exe)
        ref_path = os.path.join(BASE, 'ref', scene + '.txt')
        head = f'{scene:>9}: {len(hashes)} hashes, {"ended" if ended else "NO MOVIE END"}, interpreted {interp}, {time.time() - t0:.0f} s'
        if a.update:
            if not ended or not hashes:
                print(head, '-> not recorded', flush=True)
                failed.append(scene)
                continue
            with open(ref_path, 'w') as f:
                f.write(f'interpreted {interp}\n' + '\n'.join(hashes) + '\n')
            print(head, '-> reference recorded', flush=True)
            continue
        if not os.path.exists(ref_path):
            print(head, '-> no reference (run with --update)', flush=True)
            failed.append(scene)
            continue
        lines = open(ref_path).read().split('\n')
        ref_interp, ref = lines[0].split()[1], [l for l in lines[1:] if l]
        first = next((i for i, (x, y) in enumerate(zip(hashes, ref)) if x != y), None)
        problems = []
        if not ended:
            problems.append('movie did not end')
        if len(hashes) != len(ref):
            problems.append(f'{len(hashes)} hashes, reference {len(ref)}')
        if first is not None:
            problems.append(f'first difference at hash {first}: {hashes[first]} | ref {ref[first]}')
        if interp != ref_interp:
            problems.append(f'interpreted {interp}, reference {ref_interp}')
        print(head, '-> OK' if not problems else '-> FAIL: ' + '; '.join(problems), flush=True)
        if problems:
            failed.append(scene)
    print('REGRESS', 'FAIL ' + ','.join(failed) if failed else 'OK')
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
