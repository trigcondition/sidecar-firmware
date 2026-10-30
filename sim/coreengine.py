"""The engine core on the Mac (core/build/libcore.dylib: make -C core engines lib) with rmp_install.Link's shape:
report(sysex), frames(n), plus idle() for the work between requests and take_sysex() for what it sent the Rytm.
One engine per process (the core keeps module state, as on the Teensy): run each scenario in its own process.
"""
import ctypes
import os

import numpy as np

_LIB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "core", "build", "libcore.dylib")


class CoreEngine:
    def __init__(self, card_dir, um=False):
        self.lib = ctypes.CDLL(_LIB)
        self.lib.core_c_begin.argtypes = [ctypes.c_char_p]
        self.lib.core_c_sysex.argtypes = [ctypes.c_char_p, ctypes.c_int]
        self.lib.core_c_render.argtypes = [ctypes.POINTER(ctypes.c_int16), ctypes.c_int]
        self.lib.core_c_take_sysex.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_int]
        self.card_ok = bool(self.lib.core_c_begin(card_dir.encode()))
        if um:   # the user machines (core/user/um.h): the card's /Plugins, as the Pi
            self.lib.core_c_um_begin()
            self.idle()

    def um_command(self, c):
        """A user-machine console command ("um", "um=rescan", "um=assign:S:ID", "um=clear:S") -> its answer."""
        b = ctypes.create_string_buffer(200)
        self.lib.core_c_um_command(c.encode(), b, 200)
        return b.value.decode()

    def poly_command(self, c):
        """The poly / seq / inst console (core_c_poly_command: "poly", "seq=...", "inst", "inst=on:V:N:VEL:VOICES") ->
        its answer."""
        b = ctypes.create_string_buffer(2000)
        self.lib.core_c_poly_command(c.encode(), b, 2000)
        return b.value.decode()

    def um_json(self):
        """The user machines' status (a list of dicts, docs/user-machines-pi.md "JSON")."""
        import json
        b = ctypes.create_string_buffer(65536)
        n = self.lib.core_c_um_json(b, 65536)
        return json.loads(b.value[:n].decode()) if n else []

    def lists_command(self, c):
        """A pool / JUNO / FM list command (core/user/lists.h: pool=S:path, juno=ren:S:name, fm=save:V, ...) -> its
        answer."""
        b = ctypes.create_string_buffer(200)
        self.lib.core_c_lists_command(c.encode(), b, 200)
        return b.value.decode()

    def lists_json(self):
        """The project's pool and JUNO / FM lists (a dict, core/user/lists.h "JSON")."""
        import json
        b = ctypes.create_string_buffer(65536)
        n = self.lib.core_c_lists_json(b, 65536)
        return json.loads(b.value[:n].decode()) if n else {}

    def report(self, sysex):
        """A whole SysEx F0 .. F7 from the Rytm."""
        b = bytes(sysex)
        self.lib.core_c_sysex(b, len(b))
        self.idle(0)

    def idle(self, secs=0.0):
        """The between-requests work until none is left (secs: ignored - the Mac does it at once)."""
        while self.lib.core_c_idle(64) == 64:
            pass

    def frames(self, n):
        """n frames x 8 voices as an (n, 8) int16 array."""
        out = np.zeros((n, 8), np.int16)
        self.lib.core_c_render(out.ctypes.data_as(ctypes.POINTER(ctypes.c_int16)), n)
        return out

    def take_sysex(self):
        """Every SysEx the engine sent the Rytm since the last call, in order."""
        msgs, buf = [], (ctypes.c_uint8 * 1024)()
        while True:
            k = self.lib.core_c_take_sysex(buf, 1024)
            if not k:
                return msgs
            msgs.append(bytes(buf[:k]))
