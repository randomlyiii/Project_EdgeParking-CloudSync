# -*- coding: utf-8 -*-
"""Host test for the K210 on-screen status banner in k210_fw/main.py.

Verifies the banner state machine (NO MODEL / RECOGNIZING / OK / FAIL + aging)
by feeding App.screen_status() a fake image object and inspecting what it drew.
No hardware involved.
"""
import os
import sys
import types

REPO = r"D:\Projects\Project_EdgeParking-CloudSync"
SRC = os.path.join(REPO, "k210_fw", "park_app.py")


class FakeImg:
    def __init__(self, w=320, h=240):
        self._w, self._h = w, h
        self.rects = []
        self.texts = []

    def width(self):
        return self._w

    def draw_rectangle(self, x, y, w, h, color=(0, 0, 0), fill=False):
        self.rects.append((x, y, w, h, color, fill))

    def draw_string(self, x, y, s, color=(255, 255, 255), scale=1):
        self.texts.append((x, y, s, color, scale))


def load_app():
    """exec park_app.py far enough to get the App class (no hardware touched)."""
    src = open(SRC, encoding="utf-8").read()
    cut = src.find("\n_APP = None")
    assert cut > 0, "anchor not found"
    src = src[:cut]
    # stub the MicroPython modules park_app.py imports at module level
    for name in ("sensor", "image", "lcd", "machine", "uos", "gc", "json",
                 "sys", "utime", "maix"):
        if name in sys.modules:
            continue
        sys.modules[name] = types.ModuleType(name)
    sys.modules["utime"].ticks_ms = lambda: 0
    sys.modules["utime"].ticks_diff = lambda a, b: a - b
    sys.modules["utime"].ticks_add = lambda a, b: a + b
    ns = {"__name__": "k210_main"}
    exec(compile(src, SRC, "exec"), ns)
    return ns


def banner_of(img):
    return img.texts[-1] if img.texts else None


def main():
    ns = load_app()
    App = ns["App"]
    app = App.__new__(App)              # skip __init__ (it touches the UART)
    app.status = {"kind": "ready", "ts": 0, "reason": ""}
    ns["SCREEN_STATUS"] = 1

    print("== 1. banner is drawn in every state ==")
    cases = [
        ({"kind": "ready", "ts": 0, "reason": ""}, "READY"),
        ({"kind": "recognizing", "ts": 0, "reason": ""}, "RECOGNIZING"),
        ({"kind": "ok", "ts": 0, "plate": "B12345", "conf": 0.93}, "OK B12345 0.93"),
        ({"kind": "fail", "ts": 0, "reason": "no_plate"}, "FAIL no_plate"),
    ]
    for status, want in cases:
        app.status = status
        img = FakeImg()
        app.screen_status(img)
        got = banner_of(img)
        assert got is not None, "%s drew nothing" % want
        assert want in got[2], "want %r got %r" % (want, got[2])
        assert img.rects and img.rects[-1][5] is True, "no filled backdrop"
        assert got[4] >= 2, "banner must use a large scale (got %s)" % got[4]
        print("   %-12s -> %r color=%s" % (status["kind"], got[2], got[3]))

    print("== 2. NO MODEL blinks and carries the reason ==")
    app.status = {"kind": "nomodel", "ts": 0, "reason": "sd models missing"}
    seen_on = seen_off = False
    for t in range(0, 1200, 60):
        ns["utime"].ticks_ms = (lambda v: (lambda: v))(t)
        img = FakeImg()
        app.screen_status(img)
        if img.texts:
            seen_on = True
            assert "NO MODEL" in img.texts[-1][2], img.texts[-1][2]
            assert "sd models missing" in img.texts[-1][2], img.texts[-1][2]
        else:
            seen_off = True
    assert seen_on and seen_off, "NO MODEL must blink (on=%s off=%s)" % (seen_on, seen_off)
    ns["utime"].ticks_ms = lambda: 0
    print("   blinks during a 1.2 s window and prints the reason on screen")

    print("== 3. OK shows the age of the result ==")
    ns["utime"].ticks_ms = lambda: 12000          # 12 s after the detection
    app.status = {"kind": "ok", "ts": 0, "plate": "B12345", "conf": 0.93}
    img = FakeImg()
    app.screen_status(img)
    assert banner_of(img)[2].endswith("12s"), banner_of(img)[2]
    ns["utime"].ticks_ms = lambda: 0
    print("   'OK B12345 0.93 12s' -> stale result ages instead of looking fresh")

    print("== 4. switch off ==")
    ns["SCREEN_STATUS"] = 0
    img = FakeImg()
    app.screen_status(img)
    assert not img.texts and not img.rects, "SCREEN_STATUS=0 must draw nothing"
    print("   SCREEN_STATUS=0 draws nothing")

    print("\nALL SCREEN-STATUS CHECKS PASSED")


main()
