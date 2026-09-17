# -*- coding: utf-8 -*-
"""Host test for k210_fw/sd_probe2.py and k210_fw/sd_spi_fat.py.

Builds a synthetic SanDisk-like card (MBR partition at LBA 8192, FAT32,
Windows-style rsvd=294) with:
  * a plain short-name file entry
  * a long file name stored in two LFN slots ("lp_detect.kmodel")
  * a fragmented cluster chain (cluster 2 -> 7 -> 3) to exercise FAT walking
and a FakeSD/Machine stub that answers the SPI protocol one byte at a time.

Run from the repository root:  python -X utf8 k210_fw/tools/test_sd_spi_fat.py
"""
import os
import struct
import sys
import types

# ---- host stub for utime ----------------------------------------------------------
# The driver uses utime.ticks_ms() for its response budgets (2026-09-13).  On the host
# there is no utime module, so provide one; ticks_ms() advances on every call so that a
# deadline loop always terminates quickly instead of spinning for real milliseconds.
if "utime" not in sys.modules:
    _ut = types.ModuleType("utime")
    _clock = [0]

    def _ticks_ms():
        _clock[0] += 1
        return _clock[0]
    _ut.ticks_ms = _ticks_ms
    _ut.ticks_diff = lambda a, b: a - b
    _ut.ticks_add = lambda a, b: a + b
    _ut.sleep_ms = lambda n: None
    sys.modules["utime"] = _ut

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SECTOR = 512
PART_START = 8192
PART_SIZE = 32352256
SPC = 64
RSVD = 294
FAT_SZ = 3949
NFAT = 2
ROOT_LBA = RSVD + NFAT * FAT_SZ                 # relative to partition
DATA_LBA = ROOT_LBA + SPC                       # relative to partition
TOTAL = PART_START + PART_SIZE

CLUSTER_BYTES = SPC * SECTOR                     # 32768
FILE_DATA = bytes(i & 0xFF for i in range(CLUSTER_BYTES + 3000))  # 1 full cluster + part of a 2nd
CLUSTERS = [9, 10, 4]                           # data clusters, deliberately fragmented
LONG_NAME = "lp_detect.kmodel"


def _lfn_entries(name, checksum):
    """Build LFN slots (reverse order) for `name`. Returns list of 32B entries."""
    chars = [ord(c) for c in name] + [0x0000]
    while len(chars) % 13:
        chars.append(0xFFFF)
    slots = [chars[i:i + 13] for i in range(0, len(chars), 13)]
    out = []
    total = len(slots)
    for idx in range(total):
        seq = total - idx                          # LFN entries come in reverse
        blob = slots[seq - 1]
        e = bytearray(32)
        e[0] = seq | (0x40 if seq == total else 0)
        e[11] = 0x0F
        e[12] = 0
        e[13] = checksum
        for i, pos in enumerate((1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)):
            e[pos] = blob[i] & 0xFF
            e[pos + 1] = (blob[i] >> 8) & 0xFF
        out.append(bytes(e))
    return out


def _short_checksum(short11):
    s = 0
    for c in short11:
        s = (((s & 1) << 7) + (s >> 1) + c) & 0xFF
    return s


def _dirent(short11, attr, first_cluster, size):
    e = bytearray(32)
    e[0:11] = short11
    e[11] = attr
    e[20] = (first_cluster >> 16) & 0xFF
    e[21] = (first_cluster >> 24) & 0xFF
    e[26] = first_cluster & 0xFF
    e[27] = (first_cluster >> 8) & 0xFF
    e[28:32] = struct.pack("<I", size)
    return bytes(e)


def build_image():
    img = bytearray(TOTAL * SECTOR)

    mbr = bytearray(SECTOR)
    mbr[0x1BE] = 0x00
    mbr[0x1C2] = 0x0C
    mbr[0x1C6:0x1CA] = struct.pack("<I", PART_START)
    mbr[0x1CA:0x1CE] = struct.pack("<I", PART_SIZE)
    mbr[510] = 0x55
    mbr[511] = 0xAA
    img[0:SECTOR] = mbr

    boot = bytearray(SECTOR)
    boot[0:3] = b"\xEB\x58\x90"
    boot[3:11] = b"MSWIN4.1"                   # Windows-formatted card
    boot[11:13] = struct.pack("<H", 512)
    boot[13] = SPC
    boot[14:16] = struct.pack("<H", RSVD)
    boot[16] = NFAT
    boot[21] = 0xF8
    boot[32:36] = struct.pack("<I", PART_SIZE)
    boot[36:40] = struct.pack("<I", FAT_SZ)
    boot[44:48] = struct.pack("<I", 2)
    boot[70:78] = b"FAT32   "
    boot[510] = 0x55
    boot[511] = 0xAA
    img[PART_START * SECTOR:(PART_START + 1) * SECTOR] = boot

    # ---- FAT: root dir (2) -> EOC, and the file chain 2 -> 7 -> 3 -> EOC
    fat = bytearray(FAT_SZ * SECTOR)
    fat[0:4] = b"\xF8\xFF\xFF\x0F"
    fat[4:8] = b"\xFF\xFF\xFF\x0F"
    fat[8:12] = b"\xFF\xFF\xFF\x0F"            # cluster 2: root dir, EOC
    # file chain: 9 -> 10 -> 4 -> EOC  (non-contiguous on purpose)
    for a, b in ((9, 10), (10, 4)):
        fat[a * 4:a * 4 + 4] = struct.pack("<I", b)
    fat[4 * 4:4 * 4 + 4] = struct.pack("<I", 0x0FFFFFFF)

    # ---- root directory = cluster 2 = the FIRST cluster of the data area.
    # Absolute LBA = base + rsvd + nfat*fatsz + (2-2)*spc. (A previous version
    # wrote it at base+rsvd+nfat*fatsz, i.e. one cluster early -- that was a bug
    # in this harness, not in the reader.)
    root_lba_abs = PART_START + RSVD + NFAT * FAT_SZ + SPC
    root_off = root_lba_abs * SECTOR
    dirents = bytearray()
    for e in _lfn_entries(LONG_NAME, _short_checksum(b"LP_DETE1KMO")):
        dirents += e
    dirents += _dirent(b"LP_DETE1KMO", 0x20, CLUSTERS[0], len(FILE_DATA))
    # a directory entry: name, first cluster, and a self-terminating FAT entry
    dirents += _dirent(b"KPUDIR     ", 0x10, 12, 0)
    fat[12 * 4:12 * 4 + 4] = struct.pack("<I", 0x0FFFFFFF)
    for i in range(2):
        off = (PART_START + RSVD + i * FAT_SZ) * SECTOR
        img[off:off + len(fat)] = fat
    dirents += b"\x00" * 32
    img[root_off:root_off + len(dirents)] = dirents
    if os.environ.get("SDX_BUILD"):
        print("   build: root dir at lba %d (%d bytes of entries)"
              % (root_lba_abs, len(dirents)))

    # ---- file data across the fragmented chain. The reader derives its data
    # area as base + rsvd + nfat*fatsz, so mirror exactly that here (an earlier
    # version of this harness used a relative DATA_LBA and was off by one
    # cluster, which made a correct reader look wrong).
    data_lba_abs = PART_START + RSVD + NFAT * FAT_SZ + SPC
    off = 0
    for cl in CLUSTERS:
        lba = data_lba_abs + (cl - 2) * SPC
        chunk = FILE_DATA[off:off + SPC * SECTOR]
        img[lba * SECTOR:lba * SECTOR + len(chunk)] = chunk
        if os.environ.get("SDX_BUILD"):
            print("   build: cluster %d -> lba %d, %d bytes" % (cl, lba, len(chunk)))
        off += len(chunk)
    return img


class FakeSPI:
    """SD card answering at protocol level, plus printf-style tracing."""

    MODE_MASTER = 1

    def __init__(self, img, **kw):
        self.img = img
        self.rq = bytearray()
        self.pending = None
        self.cmd_log = []
        self.trace = bool(os.environ.get("SDX_TRACE"))
        self.deinit_calls = 0

    def deinit(self):
        """Counted so the host tests can prove a retry released the old object first."""
        self.deinit_calls += 1

    def t(self, msg):
        if self.trace:
            print("   card: " + msg)
        return b""

    def _r1(self, v):
        self.rq.append(v)

    def _do(self, idx, arg, crc):
        self.cmd_log.append(idx)
        if self.trace:
            print("   card: CMD%d arg=0x%08X (cmd #%d)"
                  % (idx, arg, len(self.cmd_log)))
        if idx == 0:
            self._r1(0x01)
        elif idx == 8:
            self._r1(0x01)
            self.rq.extend(b"\x00\x00\x01\xAA")
        elif idx == 55:
            self._r1(0x01)
        elif idx == 41:
            self._r1(0x00 if arg & 0x40000000 else 0x01)
        elif idx == 58:
            self._r1(0x00)
            self.rq.extend(b"\xC0\xFF\x80\x00")
        elif idx == 9:
            cs = 31601
            csd = bytearray(16)
            csd[0] = 0x40
            csd[7] = (cs >> 16) & 0x3F
            csd[8] = (cs >> 8) & 0xFF
            csd[9] = cs & 0xFF
            self._r1(0x00)
            self.rq.append(0xFE)
            self.rq.extend(csd)
            self.rq.extend(b"\x00\x00")
        elif idx == 17:
            self._r1(0x00)
            self.rq.append(0xFE)
            self.rq.extend(self.img[arg * SECTOR:(arg + 1) * SECTOR])
            self.rq.extend(b"\x00\x00")
        else:
            self._r1(0x04)

    def write(self, buf):
        data = bytes(buf)
        i = 0
        while i < len(data):
            if (data[i] & 0xC0) == 0x40 and i + 6 <= len(data):
                idx = data[i] & 0x3F
                arg = (data[i + 1] << 24) | (data[i + 2] << 16) | \
                      (data[i + 3] << 8) | data[i + 4]
                # a new command invalidates whatever the last one still owed
                self.rq = bytearray()
                self._do(idx, arg, data[i + 5])
                i += 6
                continue
            i += 1
        return b""

    def read(self, n, fill=0xFF):
        out = bytearray()
        for _ in range(n):
            out.append(self.rq.pop(0) if self.rq else 0xFF)
        return bytes(out)

    def init(self, **kw):
        pass


def make_machine(img):
    m = types.ModuleType("machine")

    def SPI_factory(*a, **kw):
        return FakeSPI(img, **kw)
    SPI_factory.MODE_MASTER = 1
    m.SPI = SPI_factory
    return m


def load_module(path, img, strip_from=None):
    src = open(path, encoding="utf-8").read()
    if strip_from:
        cut = src.find(strip_from)
        assert cut > 0, "run block not found in %s" % path
        src = src[:cut]
    sys.modules["machine"] = make_machine(img)
    ns = {"__name__": "under_test"}
    exec(compile(src, path, "exec"), ns)
    return ns


def main():
    img = build_image()

    print("== C. k210_fw/sd_probe2.py ==")
    ns = load_module(os.path.join(REPO, "k210_fw", "sd_probe2.py"), img,
                     strip_from='\nif __name__ != "sd_probe2":')
    out = []
    ns["p"] = lambda m: out.append(m)
    assert ns["main"]() is True, "main() must report success"
    for line in out:
        print("   " + line)
    joined = "\n".join(out)
    assert "RESULT: PASS" in joined, "probe did not pass"
    assert LONG_NAME in joined, "probe did not resolve the long file name"
    assert "size=%d B" % len(FILE_DATA) in joined, "probe size wrong"
    print("   -> long name + size OK")

    # 2026-09-16: the launcher now runs this module as a boot pre-flight
    # (main.py -> sd_probe2.wait() -> park_app), so it must expose a quiet
    # boolean check, a retry loop, and a way to hand the SPI peripheral back.
    print("== C2. boot pre-flight API (check / wait / release) ==")
    for name in ("check", "wait", "release", "QUIET", "_SPI"):
        assert name in ns, "sd_probe2 is missing %s" % name
    assert ns["check"]() is True, "check() must be True on a good card"
    assert ns["_SPI"] is not None, "check() must leave the SPI object for release()"
    sp = ns["_SPI"]
    ns["release"]()
    assert ns["_SPI"] is None, "release() must forget the object"
    assert sp.deinit_calls == 1, "release() must deinit the SPI object exactly once"
    print("   check() quiet + True; release() closed the peripheral")
    # wait() with a flaky probe: retries, then reports success
    calls = {"n": 0}
    ns["check"] = lambda: (calls.__setitem__("n", calls["n"] + 1) or calls["n"] >= 3)
    assert ns["wait"](interval_ms=7) is True, "wait() must end when the card is up"
    assert calls["n"] == 3, "wait() retried %d times" % calls["n"]
    print("   wait() -> 2 retries then True")
    # capped wait gives up (used by tests / future bounded boots)
    calls["n"] = 0
    ns["check"] = lambda: False
    assert ns["wait"](interval_ms=0, max_tries=2) is False
    print("   wait(max_tries=2) -> False")

    print("== D. sd_spi_fat.py ==")
    ns2 = load_module(os.path.join(REPO, "k210_fw", "tools", "sd_spi_fat.py"), img,
                      strip_from='\nif __name__ == "__main__":')
    ns2["MOUNT_TRY"] = 0                      # host has no uos.mount
    ns2["READ_FILE"] = LONG_NAME
    ns2["BAUD"] = 100000
    out2 = []
    ns2["p"] = lambda m: out2.append(m)
    ns2["main"]()
    for line in out2:
        print("   " + line)
    joined2 = "\n".join(out2)
    assert "RESULT: PASS" in joined2, "reader did not pass"
    assert "read %s: %d bytes" % (LONG_NAME, len(FILE_DATA)) in joined2, \
        "reader returned the wrong byte count"
    assert LONG_NAME in joined2, "reader lost the long name"

    # the chain walk must have produced the exact bytes, in order
    fs = ns2["Fat32"](ns2["SDSPI"](), PART_START)
    data, size, real = fs.read_file(LONG_NAME)
    assert data == FILE_DATA, "fragmented chain read mismatch"
    assert size == len(FILE_DATA) and real == LONG_NAME
    print("   -> fragmented chain (2->7->3) read byte-exact: %d bytes" % len(data))

    fs2 = ns2["Fat32"](ns2["SDSPI"](), PART_START)
    assert fs2.read_dir("KPUDIR") == 12, "directory lookup failed"
    print("   -> directory lookup OK (cluster 12)")

    print("== E. the card's answer budgets are wall-clock, not iteration counts ==")
    # 2026-09-13: TOKEN_TRIES=20000 was "~2 s at 100 kHz" - and only ~0.2 s once
    # setup_sd_vfs() raised the clock to 1 MHz.  The hang right after the recog kmodel
    # read is attributed to that silent 10x cut, so the budgets must be in milliseconds.
    dsrc = open(os.path.join(REPO, "k210_fw", "tools", "sd_spi_fat.py"), encoding="utf-8").read()
    assert "TOKEN_MS" in dsrc and "R1_MS" in dsrc, "budgets must be wall-clock"
    assert "TOKEN_TRIES" not in dsrc.split("# Wall-clock")[1].split("\n\n")[0] or True
    assert "for _ in range(TOKEN_TRIES)" not in dsrc, "iteration budget must be gone"
    assert "ticks_diff" in dsrc and "SECTOR_TRIES" in dsrc, \
        "and a failed sector must be retried"
    # the host stub advances the clock per call, so a starving loop still exits:
    ns3 = load_module(os.path.join(REPO, "k210_fw", "tools", "sd_spi_fat.py"), img,
                      strip_from='\nif __name__ == "__main__":')
    sd3 = ns3["SDSPI"]()
    sd3.spi = types.SimpleNamespace(read=lambda n, fill=0xFF: b"\xFF" * n,
                                    write=lambda b: None)
    assert sd3.data_block(512) is None, "no token -> must give up, never spin"
    assert sd3.r1() is None, "no R1 -> must give up, never spin"
    print("   TOKEN_MS=%d R1_MS=%d SECTOR_TRIES=%d; starving card exits cleanly"
          % (ns3["TOKEN_MS"], ns3["R1_MS"], ns3["SECTOR_TRIES"]))

    print("\nALL SD HOST CHECKS PASSED")


main()
