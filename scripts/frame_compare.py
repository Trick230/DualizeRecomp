"""Compares renderers by numbers: replays recorded inputs and dumps the frames.

    python scripts/frame_compare.py run <tag> [--exe <path>] [--env K=V ...] [scene ...]
    python scripts/frame_compare.py diff <a> <b> [scene ...]

Scenes come from the saved states in build/states/native and the input movies in
build/check/ubercmp. Dumps go to build/check/frames/<tag>/<scene>. Replays are
deterministic, so two runs of one build give the same frames unless the renderer
itself varies. Needs numpy for `diff`.
"""
import glob
import os
import re
import shutil
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))

CHECK = os.path.join(ROOT, "build", "check")
MOVIES = os.path.join(CHECK, "ubercmp")
OUT = os.path.join(CHECK, "frames")
STATES = os.path.join(ROOT, "build", "states", "native")
USER = os.path.join(CHECK, "users", "new")
SCENES = {
    "forest": "forest.sav",
    "gate": "gate.sav",
    "bluetrees": "bluetrees.sav",
    "dark1": "dark1.sav",
    "cutscene": "cutscene_eyes.sav",
    "boot": None,
}


def run(argv):
    from automation import Game

    tag = argv.pop(0)
    exe, env, scenes = None, {}, []
    while argv:
        arg = argv.pop(0)
        if arg == "--exe":
            exe = argv.pop(0)
        elif arg == "--env":
            key, _, value = argv.pop(0).partition("=")
            env[key] = value
        else:
            scenes.append(arg)
    for scene in scenes or SCENES:
        directory = os.path.join(OUT, tag, scene)
        shutil.rmtree(directory, ignore_errors=True)
        os.makedirs(directory)
        log = os.path.join(directory, "log.txt")
        old = dict(os.environ)
        os.environ.update(env)
        os.environ.update({"AURORA_FRAME_DUMP": directory, "DUALIZE_GX_STATS": "1"})
        game = Game(ROOT, exe=exe)
        try:
            state = SCENES[scene]
            game.start(state=os.path.join(STATES, state) if state else None, log=log,
                       extra_args=["--user", USER, "--no-interpolation",
                                   "--replay-input", os.path.join(MOVIES, scene + ".movie")])
            deadline = time.time() + 300
            while game.proc.poll() is None and time.time() < deadline:
                time.sleep(0.25)
            timed_out = game.proc.poll() is None
        finally:
            game.stop()
            os.environ.clear()
            os.environ.update(old)
        text = open(log, errors="replace").read()
        hashes = os.path.join(directory, "hashes.txt")
        frames = sum(1 for _ in open(hashes)) if os.path.exists(hashes) else 0
        errors = [line for line in text.splitlines() if re.search(r"error|fatal|panic|validation", line, re.I)]
        print(scene, "timeout" if timed_out else "ended", "frames", frames, "errors", len(errors),
              [e[:200] for e in errors[:2]], flush=True)


def diff(argv):
    import numpy as np

    a, b = argv[0], argv[1]
    for scene in argv[2:] or SCENES:
        da, db = os.path.join(OUT, a, scene), os.path.join(OUT, b, scene)
        ha = dict(line.split() for line in open(os.path.join(da, "hashes.txt")))
        hb = dict(line.split() for line in open(os.path.join(db, "hashes.txt")))
        common = sorted(set(ha) & set(hb), key=int)
        same = sum(1 for key in common if ha[key] == hb[key])
        first = next((key for key in common if ha[key] != hb[key]), None)
        print(scene, "frames", len(ha), len(hb), "identical", same, "/", len(common), "first differing", first)
        means, fractions, worst = [], [], (0, None)
        for path in sorted(glob.glob(os.path.join(da, "*.raw"))):
            other = os.path.join(db, os.path.basename(path))
            if not os.path.exists(other):
                continue
            x = np.fromfile(path, dtype=np.uint8).astype(np.int16)
            y = np.fromfile(other, dtype=np.uint8).astype(np.int16)
            if x.size != y.size:
                continue
            delta = np.abs(x - y).reshape(-1, 4)[:, :3]
            means.append(delta.mean())
            fractions.append((delta.max(axis=1) > 2).mean() * 100)
            if delta.max() > worst[0]:
                worst = (int(delta.max()), os.path.basename(path))
        if means:
            print("   raw frames %d, mean diff max %.4f, pixels off by >2: max %.3f%%, largest %s" % (
                len(means), max(means), max(fractions), worst))


if sys.argv[1] == "run":
    run(sys.argv[2:])
elif sys.argv[1] == "diff":
    diff(sys.argv[2:])
