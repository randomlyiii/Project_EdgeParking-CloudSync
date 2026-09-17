# -*- coding: utf-8 -*-
"""Host test for park_app.py's SD bring-up (setup_sd_vfs) and the inlined driver.

2026-09-13 change: park_app.py now carries the SPI-FAT driver INSIDE it (spliced from
sd_spi_fat.py by k210_fw/tools/build_main.py).  Reason: this board's card socket is
wired to SPI (IO26-29) while the vendor firmwares look for SDIO, so no firmware mounts
it - and a second file on /flash kept being wiped by firmware flashes (and /flash cannot
even create files here).  One self-contained park_app.py cannot be lost that way.

So this test checks two things:
  1. sd_spi_fat.setup_sd_vfs() still works (it stays as the external fallback), and
  2. park_app.py's own setup_sd_vfs() mounts via the INLINED driver classes, using the
     synthetic SanDisk-like card from test_sd_spi_fat (MBR partition at LBA 8192).
"""
import os
import re
import sys
import types

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = REPO
sys.path.insert(0, os.path.join(BASE, "tools"))
import test_sd_spi_fat as H          # synthetic card builder + FakeSPI

IMAGE = H.build_image()
BEGIN = "# ==== BEGIN INLINE SD DRIVER"
END = "# ==== END INLINE SD DRIVER"
LOG = []


def module_src():
    src = open(os.path.join(BASE, "tools", "sd_spi_fat.py"), encoding="utf-8").read()
    cut = src.find('\nif __name__ == "__main__":')
    assert cut > 0
    return src[:cut]


def main_src():
    return open(os.path.join(BASE, "park_app.py"), encoding="utf-8").read()


def inline_driver_src():
    src = main_src()
    a = src.index(BEGIN) + len(BEGIN)
    b = src.index(END)
    return src[a:b]


def main_setup_src():
    """setup_sd_vfs() from park_app.py, ready to exec.

    Wrapped in `if True:` (and indented) so the body keeps its indentation without the
    test re-indenting it by hand - the earlier version stripped the def line and got the
    docstring nesting wrong -> IndentationError.
    """
    src = main_src()
    a = src.index("def setup_sd_vfs():")
    b = src.index("\ndef _kpu_fail(", a)
    lines = src[a:b].split("\n")
    indented = "\n".join(("    " + l) if l.strip() else l for l in lines)
    return "if True:\n" + indented


def make_uos(has_register_vfs=True, mode="none"):
    """Fake uos whose behaviour mirrors the firmware variants we met on the board.

    has_register_vfs=True/False - whether this firmware HAS uos.register_vfs at all
                              (this board's v1.0.4 has NOT)
    mode="register"         - register_vfs(VfsFat32, '/sd') is accepted
    mode="mount"            - register_vfs is absent/refused, uos.mount is accepted
    mode="mount_refused"    - THIS BOARD: uos.mount raises EPERM (the firmware will
                              not take a Python block device) and /sd stays dead
    mode="none"             - /sd is unusable and neither call is accepted

    state keys: registered (only on success), register_tried, mounted (attempted).

    NOTE: the flag parameter is NOT called `register_vfs` - the inner function of the
    same name would shadow it and make every firmware look like it has register_vfs.
    """
    uos = types.ModuleType("uos")
    state = {"registered": None, "register_tried": False, "mounted": None}

    def _expose(vfs):
        uos.listdir = lambda path: vfs.listdir(path)
        uos.open = lambda path, mode_="rb": vfs.open(path, mode_)
        uos.stat = lambda path: vfs.stat(path)

    def register_vfs(*args):
        state["register_tried"] = True
        if mode != "register":
            raise OSError(1)
        state["registered"] = args
        _expose(args[0] if hasattr(args[0], "listdir") else args[1])

    def mount(dev, path):
        state["mounted"] = (dev, path)
        if mode != "mount":
            raise OSError(1)            # EPERM: Python block dev not accepted
        # a firmware mount of our partition: route uos at the raw FAT32 reader
        _expose(Fat32Shim(dev))

    if has_register_vfs:                 # this board's firmware has NO register_vfs
        uos.register_vfs = register_vfs
    uos.mount = mount
    uos.listdir = lambda path: (_ for _ in ()).throw(OSError(19))   # ENODEV
    uos.mkdir = lambda path: None
    return uos, state


class Fat32Shim:
    """Stands in for 'the firmware mounted our partition' (mode='mount')."""

    def __init__(self, dev=None):
        self.dev = dev

    def listdir(self, path):
        return ["System Volume Information", "KPU"]

    def open(self, path, mode="rb"):
        return _FakeFile()

    def stat(self, path):
        return (0, 0, 0, 0, 0, 0, 35768, 0, 0, 0)


class _FakeFile:
    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False

    def read(self, n=-1):
        return b"\x00\x01\x02\x03"

    def close(self):
        pass


def load_module(has_register_vfs=True, mode="none"):
    """Run sd_spi_fat.py itself (the external fallback) on the synthetic card."""
    sys.modules["machine"] = H.make_machine(IMAGE)
    uos, state = make_uos(has_register_vfs, mode)
    sys.modules["uos"] = uos
    ns = {"__name__": "sd_spi_fat"}
    exec(compile(module_src(), "sd_spi_fat.py", "exec"), ns)
    return ns, state


def load_main_setup(has_register_vfs=True, mode="none"):
    """Run the inlined driver + park_app.py's setup_sd_vfs on the synthetic card."""
    machine_mod = H.make_machine(IMAGE)
    sys.modules["machine"] = machine_mod
    uos, state = make_uos(has_register_vfs, mode)
    sys.modules["uos"] = uos
    # park_app.py imports machine at module level; the inline block relies on that name
    # park_app.py imports utime at its top; the inlined driver needs that name in scope
    ns = {"__name__": "main_inline", "print": lambda *a: None,
          "machine": machine_mod, "utime": sys.modules["utime"]}
    exec(compile(inline_driver_src(), "inline", "exec"), ns)
    body = main_setup_src().replace("import uos", "uos = __import__('uos')")
    exec(compile(body, "setup_sd_vfs", "exec"), ns)
    return ns["setup_sd_vfs"], state, ns


def safe_listdir(uos, path):
    try:
        return uos.listdir(path)
    except Exception:
        return []


def main():
    print("== 1. external module path still works (kept as fallback) ==")
    ns, state = load_module(mode="register")
    ok = ns["setup_sd_vfs"](print_fn=lambda m: LOG.append(m))
    assert ok is True, "sd_spi_fat.setup_sd_vfs failed: %s" % (LOG,)
    assert state["registered"] is not None
    uos = sys.modules["uos"]
    assert "lp_detect.kmodel" in uos.listdir("/sd"), uos.listdir("/sd")
    with uos.open("/sd/lp_detect.kmodel", "rb") as f:
        head = f.read(4)
    print("   module mount ok; /sd/lp_detect.kmodel head=%s" % head.hex())

    print("== 2. inlined driver, firmware WITH register_vfs -> used first ==")
    setup, state, ns = load_main_setup(mode="register")
    logs = []
    ns["print"] = lambda *a: logs.append(" ".join(str(x) for x in a))
    assert setup() is True, logs
    assert state["registered"] is not None, "register_vfs was not tried first"
    assert state["mounted"] is None, "uos.mount should not run once register_vfs worked"
    names = sys.modules["uos"].listdir("/sd")
    assert "lp_detect.kmodel" in names, names
    print("   register_vfs first -> /sd -> %s" % (names[:4],))

    print("== 2b. THIS BOARD: no register_vfs -> falls back to uos.mount ==")
    setup, state, ns = load_main_setup(has_register_vfs=False, mode="mount")
    logs = []
    ns["print"] = lambda *a: logs.append(" ".join(str(x) for x in a))
    assert setup() is True, logs
    assert state["register_tried"] is False, "register_vfs must not be tried: absent"
    assert state["mounted"] is not None, "uos.mount fallback never attempted"
    assert any("no uos.register_vfs" in m for m in logs), logs
    assert any("uos.mount our BlockDev ok" in m for m in logs), logs
    print("   -> uos.mount carried it ('%s')"
          % [m for m in logs if "uos.mount our BlockDev" in m][-1])

    # ⛔ REGRESSION GUARD (2026-09-13): the mount MUST be at 100kHz, but every byte read
    # AFTER it goes through our block device one sector at a time, so the clock has to be
    # raised or a model load takes 56s/120s and looks like a dead board.  The rewritten
    # single-file version of setup_sd_vfs() silently lost the sd.set_baud(BAUD_FAST) call
    # the old external module had - that cost a board round trip.  Never again.
    # 2026-09-14: the raise is now behind BAUD_TRY (1MHz hung the card on BOTH firmwares),
    # so the tests that exercise it must arm the switch first.
    print("== 2c. with BAUD_TRY=1 it RAISES the SPI clock once /sd is usable ==")
    for mode, has_reg, tag in (("mount", False, "uos.mount path"),
                               ("register", True, "register_vfs path")):
        setup, state, ns = load_main_setup(has_register_vfs=has_reg, mode=mode)
        ns["BAUD_TRY"] = 1
        logs = []
        ns["print"] = lambda *a: logs.append(" ".join(str(x) for x in a))
        sd = ns["SDSPI"]()
        ns["SDSPI"] = lambda *a, **kw: sd          # keep hold of the one instance
        assert setup() is True, logs
        assert sd.baud == ns["BAUD_FAST"], (tag, sd.baud, ns["BAUD_FAST"])
        assert any("models read" in m for m in logs), logs
    print("   both mount paths end at BAUD_FAST=%d"
          % ns["BAUD_FAST"])

    print("== 2c2. the default must be BAUD_TRY=0: a baud change hangs this board ==")
    # Every attempt at a faster clock has ended in a C-level hang that no Python timeout
    # can catch, so each one costs a power cycle:
    #   set_baud(1MHz)      -> hung BOTH firmwares
    #   set_baud(400kHz)    -> 2 of 4 boots dead (once inside "deinit + new SPI()", once
    #                          just before the weight file was opened)
    #   set_baud(200kHz)    -> kmodel read fine (32964 ms vs 65154), then the SECOND file
    #                          open never returned (no "weight_data_size:", no SDX
    #                          heartbeat) - the same signature, 2026-09-16 board log.
    # 100 kHz has never hung => it is the only setting we ship, and 200000 is the highest
    # value that may even be *named* here (400000 is the documented flake).
    dsrc = open(os.path.join(BASE, "tools", "sd_spi_fat.py"), encoding="utf-8").read()
    assert re.search(r"^BAUD_TRY = [01]\b", dsrc, re.M), "BAUD_TRY must be a bare 0/1"
    assert re.search(r"^BAUD_TRY = 0\b", dsrc, re.M), \
        "the default must be OFF: every baud raise so far has hung this board"
    _bf = re.search(r"^BAUD_FAST = (\d+)\b", dsrc, re.M)
    assert _bf and int(_bf.group(1)) <= 200000, \
        "BAUD_FAST may not exceed the 200000 we already know can hang (400000 is worse)"
    assert '[SD] try %s at %d ...' in dsrc, \
        "each set_baud variant must announce itself before the risky call"
    setup, state, ns = load_main_setup(mode="mount")
    ns["BAUD_TRY"] = 0                 # force the safe path regardless of the default
    logs = []
    ns["print"] = lambda *a: logs.append(" ".join(str(x) for x in a))
    sd = ns["SDSPI"]()
    ns["SDSPI"] = lambda *a, **kw: sd
    assert setup() is True, logs
    assert sd.baud == ns["BAUD"], (sd.baud, ns["BAUD"])
    assert any("BAUD_TRY=0" in m for m in logs), logs
    assert not any("set_baud" in m for m in logs), logs
    print("   %s" % [m for m in logs if "BAUD_TRY=0" in m][-1])

    print("== 2d. set_baud must survive a firmware that rejects the full init kwargs ==")
    # 2026-09-13: the single `spi.init(mode=..., baudrate=..., ...)` form silently failed
    # on the board ([SD] baud stays 100000) -> model loads took 65s + 140s.  So the
    # driver must fall back to the minimal form and finally rebuild the SPI object.
    for reject, want in (("full", "init(baudrate)"), ("always", "new SPI()")):
        setup, state, ns = load_main_setup(mode="mount")
        ns["BAUD_TRY"] = 1
        seen = {"full": 0, "minimal": 0, "ctor": []}

        class StrictSPI(H.FakeSPI):
            def init(self, **kw):
                if reject == "full" and len(kw) > 1:
                    seen["full"] += 1
                    raise TypeError("this firmware rejects extra kwargs")
                if reject == "always":
                    seen["minimal"] += 1
                    raise TypeError("this firmware rejects init() entirely")
                seen["minimal"] += 1

        def factory(*a, **kw):
            seen["ctor"].append(kw.get("baudrate"))
            return StrictSPI(IMAGE, **kw)
        factory.MODE_MASTER = 1
        sys.modules["machine"].SPI = factory
        logs = []
        ns["print"] = lambda *a: logs.append(" ".join(str(x) for x in a))
        assert setup() is True, logs
        assert any(want in m for m in logs), (reject, logs)
        if reject == "full":
            assert seen["full"] == 1, seen
        else:
            assert seen["ctor"][-1] == ns["BAUD_FAST"], seen
        print("   rejects %-6s -> %s" % (reject, [m for m in logs if "set_baud" in m][-1]))

    print("== 2d2. a clock that silently does not change must NOT be accepted ==")
    # The whole reason set_baud(baud, ref) takes a reference block: this firmware's
    # spi.init(baudrate=...) can succeed and change nothing, and "the call did not raise"
    # is exactly what made us believe a 1MHz speedup was in place when it was not.
    setup, state, ns = load_main_setup(mode="mount")
    ns["BAUD_TRY"] = 1
    logs = []
    ns["print"] = lambda *a: logs.append(" ".join(str(x) for x in a))
    sd = ns["SDSPI"]()
    ns["SDSPI"] = lambda *a, **kw: sd
    good = sd.spi

    class GarblingSPI(H.FakeSPI):
        """Every byte it clocks out comes back different (a wrong/ignored clock)."""
        def read(self, n, fill=0xFF):
            out = H.FakeSPI.read(self, n, fill)
            return bytes([out[0] ^ 0xFF]) + out[1:] if out else out

    garbling = GarblingSPI(IMAGE)

    def factory(*a, **kw):
        return garbling
    factory.MODE_MASTER = 1
    sys.modules["machine"].SPI = factory

    def no_init(**kw):
        raise TypeError("this firmware has no in-place init")
    good.init = no_init          # only the rebuild path can install the new clock
    ref = sd.read_sector(0)
    assert ref is not None and sd.spi is good
    assert sd.set_baud(400000, ref) is False, "a garbling clock must be rejected"
    assert any("reads are not LBA0" in m for m in logs), logs
    assert any("clock restored to" in m for m in logs), logs
    print("   %s" % [m for m in logs if "reads are not LBA0" in m][-1])

    print("== 3. register_vfs refuses -> uos.mount still saves it ==")
    setup, state, ns = load_main_setup(mode="mount")
    logs = []
    ns["print"] = lambda *a: logs.append(" ".join(str(x) for x in a))
    assert setup() is True, logs
    assert state["register_tried"] is True, "register_vfs should be tried first"
    assert state["registered"] is None, "register_vfs must not look successful"
    assert state["mounted"] is not None, "uos.mount must still be tried"
    assert any("register_vfs failed" in m for m in logs), logs
    print("   register_vfs refused, uos.mount path used")

    print("== 4. this board's real worst case: no register_vfs + mount refuses ==")
    setup, state, ns = load_main_setup(has_register_vfs=False, mode="mount_refused")
    logs = []
    ns["print"] = lambda *a: logs.append(" ".join(str(x) for x in a))
    assert setup() is False, logs
    assert state["mounted"] is not None, "uos.mount must still be attempted"
    assert any("no uos.register_vfs" in m for m in logs), logs
    assert any("uos.mount failed" in m for m in logs), logs
    assert any("not usable" in m for m in logs), logs
    print("   -> False; logs: %s | %s" % (logs[0], logs[-1]))

    print("== 5. firmware-mount shortcut wins when /sd is already usable ==")
    setup, state, ns = load_main_setup()
    logs = []
    ns["print"] = lambda *a: logs.append(" ".join(str(x) for x in a))
    sys.modules["uos"].listdir = lambda path: ["KPU"] if path == "/sd" else []
    assert setup() is True
    assert state["mounted"] is None, "must not re-mount what the firmware mounted"
    assert any("already usable" in m for m in logs), logs
    print("   -> '%s'" % logs[0])

    print("== 6. park_app.py stays self-contained and keeps the order ==")
    msrc = main_src()
    blk = inline_driver_src()
    assert "class SDSPI" in blk and "class Fat32" in blk and "VfsFat32" in blk
    assert "BAUD =" in blk and "TOKEN_MS" in blk, "driver constants must be inlined"
    assert all(ord(c) < 128 for c in blk), "inlined driver must be ASCII"
    # the board only ever gets ONE file: park_app.py must not import our own modules
    # (the phrase may still appear in a comment explaining why - ignore comment lines)
    code_lines = [l for l in msrc.split("\n") if not l.lstrip().startswith("#")]
    code = "\n".join(code_lines)
    assert not re.search(r"^\s*(import|from)\s+sd_spi_fat", code, re.M), \
        "park_app.py must stay self-contained (the board only gets park_app.py)"
    assert "sd_ensure_mounted" in msrc, "firmware-mount fallback must stay"
    i_sd = msrc.index("sd_wait_until_mounted()")
    i_kpu = msrc.index("kpu_load()", i_sd)
    i_cam = msrc.index("cam_init()", i_kpu)
    assert i_sd < i_kpu < i_cam, "order: mount SD -> load models -> open camera"
    # 2026-09-16: a card that never shows up must STOP the boot, not be ignored - but
    # since 2026-09-16 (user) the UNBOUNDED wait belongs to k210_fw/main.py
    # (sd_probe2.wait()): there nothing is up yet, so retrying is free, whereas by the
    # time park_app boots there is a driver, a mount and a model load behind it.  This
    # loop is the fallback and must stay BOUNDED - SD_WAIT_MAX=0 would quietly turn the
    # application back into the thing the launcher exists to prevent.
    w = msrc[msrc.index("def sd_wait_until_mounted():"):msrc.index("def _kpu_fail(")]
    assert "while True:" in w, "the SD gate must loop, not fall through"
    assert "utime.sleep_ms(SD_WAIT_MS)" in w, "the SD gate must pace its retries"
    assert "if SD_WAIT_MAX and n >= SD_WAIT_MAX:" in w, \
        "giving up must happen through an explicit SD_WAIT_MAX cap"
    m = re.search(r"^SD_WAIT_MAX = (\d+)\b", msrc, re.M)
    assert m, "SD_WAIT_MAX must stay a plain literal so this test can read it"
    assert 0 < int(m.group(1)) <= 5, \
        "SD_WAIT_MAX here is a fallback and must stay small (got %s)" % m.group(1)
    # and the handshake the app used to SKIP must run before the first CMD17
    s = msrc[msrc.index("def setup_sd_vfs():"):msrc.index("def sd_wait_until_mounted():")]
    assert s.index("sd.init()") < s.index("sd.read_sector(0)"), \
        "setup_sd_vfs must handshake (CMD0/CMD8/ACMD41/CSD) before reading LBA0"
    assert '"[SD] spi init failed: %s"' in s, "handshake failure must say which step"
    # 2026-09-16: the retry loop builds a new SDSPI every round, and a second SPI object
    # on the same peripheral while the old one is open HANGS this board (the set_baud
    # lesson: only deinit+new SPI() ever worked).  Release before constructing.
    assert s.index("_sd_release()") < s.index("SDSPI()"), \
        "a retry must deinit the previous SPI object before building a new one"
    print("   inline block=%d bytes ASCII; order mount->kpu->cam ok"
          % len(blk.encode()))

    print("== 7. the SD gate really retries, paces itself and then continues ==")
    # 2026-09-16 (user): no card => keep trying and do NOT start the models/camera.
    # The gate is driven here with a flaky setup_sd_vfs() so the retry/pacing logic is
    # exercised without a board: fail twice, then succeed.
    setup, state, ns = load_main_setup()
    calls = {"n": 0}
    slept = []

    class _U:
        @staticmethod
        def sleep_ms(ms):
            slept.append(ms)
    ns["utime"] = _U

    def flaky():
        calls["n"] += 1
        return calls["n"] >= 3
    ns["setup_sd_vfs"] = flaky
    got = ns["sd_wait_until_mounted"]()
    assert got is True, "the gate must return True once /sd comes up"
    assert calls["n"] == 3, "expected two retries then success, got %d call(s)" % calls["n"]
    assert slept == [ns["SD_WAIT_MS"], ns["SD_WAIT_MS"]], slept

    # and with the cap armed it gives up instead of looping for ever
    ns["SD_WAIT_MAX"] = 2
    calls["n"] = 0
    slept[:] = []
    got = ns["sd_wait_until_mounted"]()
    assert got is False and calls["n"] == 2, (got, calls)
    print("   2 retries -> up (slept %d ms each); SD_WAIT_MAX=2 -> gives up" %
          (ns["SD_WAIT_MS"],))

    print("\nALL SETUP-SD-VFS CHECKS PASSED")


if __name__ == "__main__":
    main()
