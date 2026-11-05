"""Injecte des appuis de touches dans un serveur X (Xvfb) via l'extension XTest.
usage: DISPLAY=:77 xkeys.py <touche> [<touche> ...]
Touches : noms de keysym X11 (Return, Escape, Right, Left, Up, Down, e, q, Tab...).
Préfixe "Shift+" accepté. Chaque touche est enfoncée XKEYS_HOLD s (0.15 par défaut, sous le délai de répétition de 400 ms) puis relâchée, 400 ms entre deux."""
import ctypes
import sys
import time

x11 = ctypes.CDLL("libX11.so.6")
xtst = ctypes.CDLL("libXtst.so.6")
x11.XOpenDisplay.restype = ctypes.c_void_p
x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
x11.XStringToKeysym.restype = ctypes.c_ulong
x11.XStringToKeysym.argtypes = [ctypes.c_char_p]
x11.XKeysymToKeycode.restype = ctypes.c_ubyte
x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
x11.XFlush.argtypes = [ctypes.c_void_p]
xtst.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]

dpy = x11.XOpenDisplay(None)
if not dpy:
    sys.exit("impossible d'ouvrir le DISPLAY")


def focus_top_window():
    """Sans gestionnaire de fenêtres (Xvfb), personne ne donne le focus : on le donne à la
    dernière fenêtre de premier niveau (celle de l'application), et on la lève."""
    root = x11.XDefaultRootWindow(dpy)
    # Fenêtre de premier niveau sous le pointeur (SDL crée aussi des fenêtres auxiliaires).
    r, child = ctypes.c_ulong(), ctypes.c_ulong()
    i = ctypes.c_int()
    m = ctypes.c_uint()
    x11.XQueryPointer(dpy, root, ctypes.byref(r), ctypes.byref(child), ctypes.byref(i),
                      ctypes.byref(i), ctypes.byref(i), ctypes.byref(i), ctypes.byref(m))
    win = child.value
    if win:
        print(f"focus -> fenêtre 0x{win:x}", file=sys.stderr)
        x11.XRaiseWindow(dpy, win)
        x11.XSetInputFocus(dpy, win, 1, 0)  # RevertToPointerRoot, CurrentTime
        x11.XFlush(dpy)
        time.sleep(0.3)


x11.XDefaultRootWindow.restype = ctypes.c_ulong
x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
x11.XQueryTree.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p, ctypes.c_void_p,
                           ctypes.c_void_p, ctypes.c_void_p]
x11.XRaiseWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
x11.XQueryPointer.argtypes = [ctypes.c_void_p, ctypes.c_ulong] + [ctypes.c_void_p] * 7
x11.XSetInputFocus.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
focus_top_window()


def keycode(name):
    ks = x11.XStringToKeysym(name.encode())
    if not ks:
        sys.exit(f"keysym inconnu : {name}")
    return x11.XKeysymToKeycode(dpy, ks)


def send(kc, down):
    xtst.XTestFakeKeyEvent(dpy, kc, 1 if down else 0, 0)
    x11.XFlush(dpy)


for arg in sys.argv[1:]:
    parts = arg.split("+")
    codes = [keycode("Shift_L" if p == "Shift" else p) for p in parts]
    for c in codes:
        send(c, True)
    time.sleep(float(__import__("os").environ.get("XKEYS_HOLD", "0.15")))
    for c in reversed(codes):
        send(c, False)
    time.sleep(0.4)
