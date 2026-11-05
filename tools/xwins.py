"""Liste les fenêtres de premier niveau d'un serveur X : id, état, géométrie, nom."""
import ctypes
x = ctypes.CDLL("libX11.so.6")
x.XOpenDisplay.restype = ctypes.c_void_p; x.XOpenDisplay.argtypes = [ctypes.c_char_p]
x.XDefaultRootWindow.restype = ctypes.c_ulong; x.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
x.XQueryTree.argtypes = [ctypes.c_void_p, ctypes.c_ulong] + [ctypes.c_void_p] * 4
x.XFetchName.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p]


class Attr(ctypes.Structure):
    _fields_ = [("x", ctypes.c_int), ("y", ctypes.c_int), ("w", ctypes.c_int), ("h", ctypes.c_int),
                ("border", ctypes.c_int), ("depth", ctypes.c_int), ("visual", ctypes.c_void_p),
                ("root", ctypes.c_ulong), ("cls", ctypes.c_int), ("bit_gravity", ctypes.c_int),
                ("win_gravity", ctypes.c_int), ("backing_store", ctypes.c_int),
                ("backing_planes", ctypes.c_ulong), ("backing_pixel", ctypes.c_ulong),
                ("save_under", ctypes.c_int), ("colormap", ctypes.c_ulong),
                ("map_installed", ctypes.c_int), ("map_state", ctypes.c_int),
                ("all_event_masks", ctypes.c_long), ("your_event_mask", ctypes.c_long),
                ("do_not_propagate_mask", ctypes.c_long), ("override_redirect", ctypes.c_int),
                ("screen", ctypes.c_void_p)]


x.XGetWindowAttributes.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(Attr)]
d = x.XOpenDisplay(None)
r, p = ctypes.c_ulong(), ctypes.c_ulong()
ch, n = ctypes.POINTER(ctypes.c_ulong)(), ctypes.c_uint()
x.XQueryTree(d, x.XDefaultRootWindow(d), ctypes.byref(r), ctypes.byref(p), ctypes.byref(ch), ctypes.byref(n))
for i in range(n.value):
    w = ch[i]; a = Attr(); x.XGetWindowAttributes(d, w, ctypes.byref(a))
    name = ctypes.c_char_p(); x.XFetchName(d, w, ctypes.byref(name))
    print(f"0x{w:x} map={a.map_state} {a.x},{a.y} {a.w}x{a.h} events=0x{a.your_event_mask:x} "
          f"override={a.override_redirect} name={name.value}")
