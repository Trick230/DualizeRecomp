"""Client for moderngekko-run's automation directory (--automation-dir).

Each command is a key=value file dropped into <dir>/commands; the runtime
moves it to processed/ or failed/ once applied.
"""
import os
import struct
import subprocess
import time
from pathlib import Path


class AutomationError(RuntimeError):
    pass


class Game:
    def __init__(self, repo, automation_dir=None, scratch=None):
        self.repo = Path(repo)
        self.dir = Path(automation_dir or self.repo / "build" / "automation")
        self.scratch = Path(scratch or self.dir / "scratch")
        self.scratch.mkdir(parents=True, exist_ok=True)
        self.seq = 0
        self.proc = None
        self.thread = 0

    # -- process ---------------------------------------------------------
    def start(self, module_dll, state=None, log=None, extra_args=(), game=None):
        self.stop()
        for sub in ("commands", "processed", "failed"):
            (self.dir / sub).mkdir(parents=True, exist_ok=True)
            for f in (self.dir / sub).iterdir():
                f.unlink()
        args = [str(self.repo / "lib/ModernGekko/build/moderngekko-run.exe"),
                "--game", str(game or self.repo / "extracted/nights"), "--module", str(module_dll),
                "--graphics", "Native", "--automation-dir", str(self.dir), *extra_args]
        if state:
            args += ["--load-state", str(state)]
        (self.dir / "status.txt").unlink(missing_ok=True)
        env = dict(os.environ)
        env["PATH"] = str(self.repo.parent / "msys64/ucrt64/bin") + os.pathsep + env["PATH"]
        out = open(log, "w") if log else subprocess.DEVNULL
        self.proc = subprocess.Popen(args, stdout=out, stderr=subprocess.STDOUT, cwd=self.repo,
                                     env=env)
        deadline = time.time() + 120
        while time.time() < deadline:
            status = self.status()
            if status.get("booted") == "1" and status.get("state") == "running":
                return
            time.sleep(0.5)
        raise AutomationError("game did not boot")

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
        self.proc = None
        subprocess.run(["taskkill", "/F", "/IM", "moderngekko-run.exe"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def status(self):
        try:
            text = (self.dir / "status.txt").read_text()
        except OSError:
            return {}
        return dict(line.split("=", 1) for line in text.splitlines() if "=" in line)

    # -- commands --------------------------------------------------------
    def command(self, name, timeout=120, **fields):
        self.seq += 1
        file_name = f"{self.seq:05d}_{name}"
        tmp = self.dir / "command.tmp"  # same volume as commands/, so the rename is atomic
        tmp.write_text("".join(f"{k}={v}\n" for k, v in {"command": name, **fields}.items()))
        os.replace(tmp, self.dir / "commands" / file_name)
        deadline = time.time() + timeout
        while time.time() < deadline:
            if (self.dir / "processed" / file_name).exists():
                return
            if (self.dir / "failed" / file_name).exists():
                raise AutomationError(self.status().get("last_error", file_name))
            if self.proc and self.proc.poll() is not None:
                raise AutomationError("game exited")
            time.sleep(0.02)
        raise AutomationError(f"{file_name} timed out")

    def pause(self):
        self.command("pause")

    def resume(self):
        self.command("resume")

    def pad(self, frames=0, port=0, **controls):
        """Holds the named controls (classic_lx=-1.0, classic_a=1, ...) for
        `frames` frames, or until the next pad command when frames is 0.
        Controls left out are released."""
        if frames:
            self.command("pad_frames", port=port, frames=frames, **controls)
        else:
            self.command("pad", port=port, **controls)

    def save_state(self, path):
        self.command("save_state", path=Path(path).resolve())

    def screenshot(self, path):
        """Captures the game window (the runtime's own screenshot needs the
        frame dumper, which the native GX backend does not feed)."""
        subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                        str(self.repo / "scripts/window_shot.ps1"), "-Out", str(Path(path).resolve())],
                       check=True, stdout=subprocess.DEVNULL)

    def read(self, address, size):
        out = self.scratch / "read.bin"
        self.command("read_memory", address=f"0x{address:08X}", size=size, path=out)
        return out.read_bytes()

    def read32(self, address):
        return struct.unpack(">I", self.read(address, 4))[0]

    def write(self, address, data):
        self.command("write_memory", address=f"0x{address:08X}", data=data.hex())

    def write32(self, address, *values):
        self.write(address, struct.pack(f">{len(values)}I", *values))

    def call(self, address, *args, thread=None, run_async=False):
        """Runs a guest function and returns r3. A paused call runs on the
        interpreter while the game is stopped; an async one diverts the guest
        thread and lets the game run, for callees that wait on hardware."""
        out = self.scratch / "call.txt"
        fields = {"address": f"0x{address:08X}", "path": out}
        if args:
            fields["args"] = ",".join(f"0x{a & 0xFFFFFFFF:X}" for a in args)
        thread = self.thread if thread is None else thread
        if thread:
            fields["thread"] = f"0x{thread:08X}"
        if run_async:
            fields["async"] = 1
        self.command("call", timeout=200 if run_async else 120, **fields)
        values = dict(l.split("=", 1) for l in out.read_text().splitlines())
        return int(values["r3"], 16)
