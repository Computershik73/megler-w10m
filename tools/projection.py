r"""The phone's screen, and taps on it, through the WP8.1 projection client (MyProjectionClient.exe) running on this PC.

    python tools/projection.py shot OUT.png            the client area (the phone's screen) as a PNG; says if it is dark
    python tools/projection.py tap X Y [--post]        a tap at X, Y of that picture (SendInput; --post: window messages)
    python tools/projection.py swipe X1 Y1 X2 Y2 [MS]  a drag from one point to another over MS milliseconds (default 300)
    python tools/projection.py info                    the window's title, its client size and where it is on the screen

The client mirrors the phone's screen over USB and turns the mouse into touches. Drive the window someone already
started: a new projection session makes the phone ask its user for access to the screen. Coordinates are those of the
picture `shot` saves (the client area, 1:1). A phone whose screen is off stays dark: a tap does not wake it.
"""
import ctypes
import ctypes.wintypes as W
import sys
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)

try:
    user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))  # per-monitor v2: real pixels
except Exception:
    try:
        user32.SetProcessDPIAware()
    except Exception:
        pass

EnumWindowsProc = ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP = 2, 4


def find_window():
    found = []

    def cb(hwnd, _):
        n = user32.GetWindowTextLengthW(hwnd)
        if n:
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(hwnd, buf, n + 1)
            if "Projection Client" in buf.value and user32.IsWindowVisible(hwnd):
                found.append((hwnd, buf.value))
        return True

    user32.EnumWindows(EnumWindowsProc(cb), 0)
    if not found:
        sys.exit("no projection client window")
    return found[0]


def client_size(hwnd):
    r = W.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(r))
    return r.right - r.left, r.bottom - r.top


def client_origin(hwnd):
    p = W.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(p))
    return p.x, p.y


def to_foreground(hwnd):
    # Windows lets a process bring a window forward after it sent an input of its own (an Alt press here).
    user32.keybd_event(0x12, 0, 0, 0)
    user32.keybd_event(0x12, 0, 2, 0)
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.15)


def shot(path):
    """The client area as the screen shows it, the window brought forward first: PrintWindow gives black, as the
    client draws with Direct2D."""
    from PIL import ImageGrab
    hwnd, _ = find_window()
    to_foreground(hwnd)
    time.sleep(0.3)
    ox, oy = client_origin(hwnd)
    cw, ch = client_size(hwnd)
    picture = ImageGrab.grab(bbox=(ox, oy, ox + cw, oy + ch), all_screens=True)
    picture.save(path)
    dark = picture.crop((4, 4, cw - 4, ch - 4)).convert("L").getextrema()[1] < 16  # (the client's frame is grey)
    print(f"{path}: {cw}x{ch}" + (" (dark: the phone's screen is off)" if dark else ""))


def tap(x, y, post=False):
    hwnd, _ = find_window()
    if post:
        lp = (int(y) << 16) | (int(x) & 0xFFFF)
        user32.PostMessageW(hwnd, 0x0200, 0, lp)  # WM_MOUSEMOVE
        user32.PostMessageW(hwnd, 0x0201, 1, lp)  # WM_LBUTTONDOWN, MK_LBUTTON
        time.sleep(0.08)
        user32.PostMessageW(hwnd, 0x0202, 0, lp)  # WM_LBUTTONUP
        print(f"posted a tap at {x},{y}")
        return
    to_foreground(hwnd)
    ox, oy = client_origin(hwnd)
    user32.SetCursorPos(int(ox + x), int(oy + y))
    time.sleep(0.05)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.08)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    print(f"tapped {x},{y}")


def swipe(x1, y1, x2, y2, ms=300):
    hwnd, _ = find_window()
    to_foreground(hwnd)
    ox, oy = client_origin(hwnd)
    steps = max(8, int(ms / 16))
    user32.SetCursorPos(int(ox + x1), int(oy + y1))
    time.sleep(0.05)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    for i in range(1, steps + 1):
        t = i / steps
        user32.SetCursorPos(int(ox + x1 + (x2 - x1) * t), int(oy + y1 + (y2 - y1) * t))
        time.sleep(ms / 1000 / steps)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    print(f"swiped {x1},{y1} -> {x2},{y2}")


if __name__ == "__main__":
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    if a[0] == "shot":
        shot(a[1])
    elif a[0] == "tap":
        tap(float(a[1]), float(a[2]), post="--post" in a)
    elif a[0] == "swipe":
        swipe(float(a[1]), float(a[2]), float(a[3]), float(a[4]), float(a[5]) if len(a) > 5 else 300)
    elif a[0] == "info":
        hwnd, title = find_window()
        print(title, client_size(hwnd), client_origin(hwnd))
    else:
        sys.exit(__doc__)
