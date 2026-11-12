# Fullscreen on/off with real Alt+Enter, with the user's settings (users/q):
# after each toggle prints the window's client size and the size the frames
# are rendered at (the frame dump's), which should follow it.
import ctypes, glob, os, shutil, sys, time
from ctypes import wintypes
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..')).replace(os.sep, '/')
sys.path.insert(0, ROOT + '/scripts')
from automation import Game
user32 = ctypes.windll.user32
user32.SetProcessDPIAware()
user32.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
user32.SetForegroundWindow.argtypes = [wintypes.HWND]
user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]


def window(pid):
    found = []
    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def each(hwnd, _):
        p = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(hwnd):
            r = wintypes.RECT()
            user32.GetWindowRect(hwnd, ctypes.byref(r))
            found.append(((r.right - r.left) * (r.bottom - r.top), hwnd))
        return True
    user32.EnumWindows(each, 0)
    return max(found)[1] if found else None  # the game's window, not a helper


def alt_enter(hwnd):
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.3)
    VK_MENU, VK_RETURN, UP = 0x12, 0x0D, 2
    user32.keybd_event(VK_MENU, 0, 0, 0)
    user32.keybd_event(VK_RETURN, 0, 0, 0)
    time.sleep(0.05)
    user32.keybd_event(VK_RETURN, 0, UP, 0)
    user32.keybd_event(VK_MENU, 0, UP, 0)


user = ROOT + '/build/check/users/q'
d = ROOT + '/build/check/frames/fq'
shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
os.environ['AURORA_FRAME_DUMP'] = d
os.environ['AURORA_FRAME_DUMP_EVERY'] = '10'


def report(hwnd, tag):
    r = wintypes.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(r))
    raws = sorted(glob.glob(d + '/*.raw'), key=os.path.getmtime)
    n = os.path.getsize(raws[-1]) // 4 if raws else 0
    w = int(round((n * 16 / 9) ** 0.5)) if n else 0
    line = [l for l in open(user + '/graphics.ini') if l.startswith('fullscreen')]
    print(tag, 'client', r.right, 'x', r.bottom, '| rendered', w, 'x', n // w if w else 0, '|', line[0].strip() if line else '?', flush=True)
    for f in raws[:-2]:
        try: os.remove(f)
        except OSError: pass


g = Game(ROOT)
try:
    g.start(state=ROOT + '/build/states/native/forest.sav', log=ROOT + '/build/check/fullscreen_q.log',
            extra_args=['--user', user], hidden=False)
    time.sleep(7)
    hwnd = window(g.proc.pid)
    user32.SetForegroundWindow(hwnd); time.sleep(1)
    report(hwnd, 'start  ')
    for i in range(4):
        alt_enter(hwnd); time.sleep(2.5); report(hwnd, f'after {i + 1}')
    if os.environ.get('FQ_MAXIMIZE'):
        alt_enter(hwnd); time.sleep(2.5); report(hwnd, 'windowed')
        user32.ShowWindow(hwnd, 3); time.sleep(2.5); report(hwnd, 'maximized')
        user32.ShowWindow(hwnd, 9); time.sleep(2.5); report(hwnd, 'restored')
finally:
    g.stop()
