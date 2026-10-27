"""Client for DualizeRecomp's automation directory (--automation-dir).

Each command is a key=value file dropped into <dir>/commands; the runtime runs
them in name order and deletes each once it has been applied.
"""
import os
import subprocess
import time
from pathlib import Path


class AutomationError(RuntimeError):
    pass


class Game:
    def __init__(self, repo, automation_dir=None, exe=None):
        self.repo = Path(repo)
        self.dir = Path(automation_dir or self.repo / "build" / "automation")
        self.exe = Path(exe or self.repo / "build/release/DualizeRecomp/DualizeRecomp.exe")
        self.seq = 0
        self.proc = None

    # -- process ---------------------------------------------------------
    def start(self, state=None, log=None, extra_args=(), game=None):
        self.stop()
        commands = self.dir / "commands"
        commands.mkdir(parents=True, exist_ok=True)
        for f in commands.iterdir():
            f.unlink()
        args = [str(self.exe), "--game", str(game or self.repo / "extracted/nights"),
                "--automation-dir", str(self.dir), *extra_args]
        if state:
            args += ["--load-state", str(state)]
        out = open(log, "w") if log else subprocess.DEVNULL
        self.proc = subprocess.Popen(args, stdout=out, stderr=subprocess.STDOUT, cwd=self.repo)

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.command("stop")
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.proc = None

    # -- commands --------------------------------------------------------
    def command(self, name, timeout=120, **fields):
        """Queues a command and waits until the runtime has taken it."""
        self.seq += 1
        file_name = f"{self.seq:05d}_{name}"
        tmp = self.dir / "command.tmp"  # same volume as commands/, so the rename is atomic
        tmp.write_text("".join(f"{k}={v}\n" for k, v in {"command": name, **fields}.items()))
        target = self.dir / "commands" / file_name
        os.replace(tmp, target)
        deadline = time.time() + timeout
        while time.time() < deadline:
            if not target.exists():
                return
            if self.proc and self.proc.poll() is not None:
                raise AutomationError("game exited")
            time.sleep(0.02)
        raise AutomationError(f"{file_name} timed out")

    def pad(self, frames=0, port=0, **controls):
        """Holds the named controls (classic_lx=-1.0, classic_a=1, ...) for
        `frames` VI fields, or until the next pad command when frames is 0.
        Controls left out are released."""
        if frames:
            self.command("pad_frames", port=port, frames=frames, **controls)
        else:
            self.command("pad", port=port, **controls)

    def clear_pad(self, port=0):
        self.command("clear_pad", port=port)

    def save_state(self, path):
        self.command("save_state", path=Path(path).resolve())

    def load_state(self, path):
        self.command("load_state", path=Path(path).resolve())

    def screenshot(self, path):
        """Captures the game window."""
        subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                        str(self.repo / "scripts/window_shot.ps1"), "-Out", str(Path(path).resolve())],
                       check=True, stdout=subprocess.DEVNULL)
