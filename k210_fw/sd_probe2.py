# -*- coding: utf-8 -*-
"""K210 SD card SPI block-device probe (READ-ONLY).

Purpose: answer one question -- can this board read the card as a plain block
device over SPI, without the firmware mount layer (machine.SDCard is a stub on
this firmware: 'cannot create SDCard instances')?

What it does:
  1. CMD0 / CMD8 / ACMD41 handshake (SPI mode)
  2. CMD9  -> CSD register, decode capacity
  3. CMD17 -> LBA 0 (partition table), check the 0x55AA signature
  4. CMD17 -> the partition's boot sector, walk FAT32 geometry, list the root dir

How to judge the result:
  * Run it twice -- once with the card inserted, once with the card REMOVED.
    The two runs MUST differ. If they are identical, those four pins are not
    wired to the card socket (you are reading a floating bus).
  * Card in: CSD must print, LBA0 must end with 55 AA, the root dir listing
    should show the files you copied (e.g. KPU/ or the three model files).

MicroPython notes: no bytes.hex() on this firmware -> hx() below; no machine.Pin
-> CS is the SPI hardware cs0.

TWO ROLES (2026-09-16):
  1. standalone: saved as /flash/main.py and run on its own -- prints everything
     and exits (that is what main() still does).
  2. boot pre-flight: imported by the launcher main.py, which calls wait() and only
     then imports park_app.  Settling the card there costs nothing (no models, no
     camera, no KPU are up yet), which is why the retry loop lives HERE and not in
     park_app.py.
"""
import machine

PINS = {"sclk": 27, "mosi": 28, "miso": 26, "cs": 29}   # from /flash/config.json
BAUD = 100000            # deliberately slow: cards that fail here may work at 400k
R1_TRIES = 256           # ~2.5 s of R1 polling at 100 kHz
TOKEN_TRIES = 20000      # ~2 s of polling for the 0xFE token: a real random read
                         # can take ~100 ms, and this card clearly takes longer
BUS_FLUSH_BYTES = 64     # clocks after every data block, to clock out the 2 CRC
                         # bytes the card still holds (state desync otherwise)
RESET_BEFORE_EACH_READ = 0   # 1 = CMD0 + re-init before every block read

R1_NAMES = {0x00: "ready", 0x01: "idle", 0x04: "illegal command",
            0x7F: "no card (bus idle)", 0xFF: "no card (bus idle)"}


def hx(buf, n=None):
    if buf is None:
        return "None"
    if n is not None:
        buf = buf[:n]
    return " ".join("0x%02X" % b for b in buf)


def _r1_text(r):
    if r is None:
        return "no response (bus idle)"
    return "0x%02X %s" % (r, R1_NAMES.get(r, "?"))


QUIET = False           # wait()/check() flip this: same logic, no console noise


def p(msg):
    if not QUIET:
        print("SD2:" + msg)


def tick():
    try:
        import utime
        return utime.ticks_ms()
    except Exception:
        return 0


def since(t0):
    try:
        import utime
        return utime.ticks_diff(utime.ticks_ms(), t0)
    except Exception:
        return -1


_SPI = None             # the SPI object we opened, so release() can close it


def open_spi():
    global _SPI
    master = getattr(machine.SPI, "MODE_MASTER", None)
    if master is None:
        master = getattr(machine.SPI, "MASTER", 1)
    _SPI = machine.SPI(1, mode=master, baudrate=BAUD, polarity=0, phase=0,
                       bits=8, sck=PINS["sclk"], mosi=PINS["mosi"],
                       miso=PINS["miso"], cs0=PINS["cs"])
    return _SPI


def release():
    """Close the SPI object we opened.

    MUST be called before anything else opens SPI on this board: a second SPI
    object on the same peripheral while the old one is still open hangs the K210
    (the set_baud() variant-3 lesson).  wait() calls it after every attempt, so
    park_app starts from a clean peripheral.
    """
    global _SPI
    s, _SPI = _SPI, None
    if s is None:
        return
    try:
        s.deinit()
    except Exception:
        pass


class Card:
    def __init__(self, spi):
        self.spi = spi

    def rb(self):
        return self.spi.read(1, 0xFF)[0]

    def frame(self, idx, arg, crc):
        self.spi.write(bytes([0x40 | idx, (arg >> 24) & 0xFF, (arg >> 16) & 0xFF,
                              (arg >> 8) & 0xFF, arg & 0xFF, crc]))

    def r1(self, tries=R1_TRIES):
        """Poll R1; 0x7F/0xFF both mean the card is not driving MISO."""
        for _ in range(tries):
            b = self.rb()
            if b != 0xFF and b != 0x7F:
                return b
        return None

    def cmd(self, idx, arg, crc=0x00, tries=R1_TRIES):
        self.frame(idx, arg, crc)
        return self.r1(tries)

    def acmd(self, idx, arg, tries=R1_TRIES):
        self.cmd(55, 0, 0x01, 8)
        return self.cmd(idx, arg, 0x00, tries)

    def flush(self, n=BUS_FLUSH_BYTES):
        """Clock out bytes the card still holds (the 2 CRC bytes after a data
        block). Skipping this desynchronises the card for the NEXT command."""
        self.spi.write(bytes([0xFF]) * n)

    def data_block(self, nbytes, token_wait=TOKEN_TRIES):
        """Wait for the 0xFE data token, then read the block. A real SD card may
        need real time here (random access can take ~100 ms), so poll generously.
        Always flush afterwards so the next command starts on a byte boundary."""
        got = None
        for _ in range(token_wait):
            b = self.rb()
            if b == 0xFE:
                got = self.spi.read(nbytes, 0xFF)
                break
            if b != 0xFF and b != 0x7F:
                break
        self.flush()
        return got

    def read_csd(self):
        r = self.cmd(9, 0)
        if r != 0x00:
            return None, r
        return self.data_block(16), r

    def read_lba(self, lba):
        if RESET_BEFORE_EACH_READ:
            # some cards stop answering CMD17 until they are re-initialised
            err = self.init()
            if err:
                return None, "re-init: " + err
        r = self.cmd(17, lba)
        if r != 0x00:
            return None, r
        return self.data_block(512), r

    def init(self):
        for _ in range(10):
            self.spi.write(b"\xFF")
        r = self.cmd(0, 0, 0x95)
        if r != 0x01:
            return "CMD0 -> %s (expect 0x01 idle)" % _r1_text(r)
        self.frame(8, 0x1AA, 0x87)
        r8 = self.r1()
        if r8 is None:
            return "CMD8 no response"
        echo = self.spi.read(4, 0xFF)
        if r8 != 0x01:
            return "CMD8 -> 0x%02X: not a v2 (SDHC) card" % r8
        if echo[2] != 0x01 or echo[3] != 0xAA:
            return "CMD8 echo bad: %s" % hx(echo)
        ready = False
        for i in range(100):
            b = self.acmd(41, 0x40000000, 4)
            if b == 0x00:
                ready = True
                p("ACMD41 ready after %d tries" % (i + 1))
                break
            if b is None:
                return "ACMD41 no response (card stuck)"
        if not ready:
            return "ACMD41 never ready"
        r = self.cmd(58, 0)
        if r == 0x00:
            ocr = self.spi.read(4, 0xFF)
            p("OCR = %s -> %s" % (hx(ocr),
                                  "SDHC/SDXC" if ocr[0] & 0x40 else "SDSC <=2GB"))
        return None


def _short_name(e):
    base = "".join(chr(c) for c in e[0:8] if c not in (0x20, 0x00))
    ext = "".join(chr(c) for c in e[8:11] if c not in (0x20, 0x00))
    return (base + "." + ext) if ext else base


def parse_dir(data):
    """Parse one 512 B directory block into a list of
    (long_name_or_None, short_name, attr, fst_cluster, size).

    Long File Name (LFN) entries carry attr 0x0F and hold the real name in
    UTF-16 chunks that precede the 8.3 entry; they MUST be reassembled or every
    name shows up truncated ('LP_DET~1.KMO' instead of 'lp_detect.kmodel')."""
    out = []
    lfn = {}
    for off in range(0, len(data) - 31, 32):
        e = data[off:off + 32]
        if e[0] == 0x00:
            break
        if e[0] == 0xE5:
            lfn = {}
            continue
        attr = e[11]
        if attr == 0x0F:
            seq = e[0] & 0x3F
            part = []
            done = False
            for i in (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30):
                if done:
                    break
                c = e[i] | (e[i + 1] << 8)
                if c == 0x0000:
                    done = True
                    break
                if c != 0xFFFF:
                    part.append(c)
            lfn[seq] = part
            continue
        name = None
        if lfn:
            seqs = sorted(lfn.keys())
            chars = []
            for s in seqs:
                chars.extend(lfn[s])
            cut = len(chars)
            for i, c in enumerate(chars):
                if c == 0:
                    cut = i
                    break
            try:
                name = "".join(chr(c) for c in chars[:cut])
            except Exception:
                name = None
            lfn = {}
        # DIR_FstClusHI (offset 20) and DIR_FstClusLO (offset 26)
        fst = (e[26] | (e[27] << 8)) | ((e[20] | (e[21] << 8)) << 16)
        size = e[28] | (e[29] << 8) | (e[30] << 16) | (e[31] << 24)
        out.append((name, _short_name(e), attr, fst, size))
    return out


def fat_list(data):
    """Human-readable listing lines for one directory block."""
    lines = []
    for name, short, attr, fst, size in parse_dir(data):
        kind = "DIR " if attr & 0x10 else "FILE"
        lines.append("%s %-24s short=%-14s first_cluster=%-6d size=%d B"
                     % (kind, name if name else "-", short, fst, size))
    if not lines:
        lines.append("(empty)")
    return lines


def parse_fat32(boot):
    if len(boot) < 512:
        return None
    bps = boot[11] | boot[12] << 8
    spc = boot[13]
    rsvd = boot[14] | boot[15] << 8
    nfat = boot[16]
    fatsz = boot[36] | boot[37] << 8 | boot[38] << 16 | boot[39] << 24
    root = boot[44] | boot[45] << 8 | boot[46] << 16 | boot[47] << 24
    tot = boot[32] | boot[33] << 8 | boot[34] << 16 | boot[35] << 24
    if bps != 512 or spc == 0 or fatsz == 0:
        return None
    if boot[21] != 0xF8 and boot[21] != 0xF0:
        return None
    return {"bps": bps, "spc": spc, "rsvd": rsvd, "nfat": nfat,
            "fatsz": fatsz, "root": root, "total": tot,
            # cluster 2 (the root directory) is the first cluster of the data
            # area, so its LBA is rsvd + nfat*fatsz + (2-2)*spc
            "root_lba": rsvd + nfat * fatsz + spc}


def main():
    p("==== SD SPI block-device probe (read-only) ====")
    card = Card(open_spi())

    err = card.init()
    if err:
        p("RESULT: FAIL - %s" % err)
        p("(run again with the card REMOVED: identical output means the pins "
          "are not wired to the socket)")
        return False
    p("handshake OK")

    t0 = tick()
    csd, r = card.read_csd()
    if csd is None:
        p("RESULT: FAIL - CMD9 R1=%s (no CSD), %d ms" % (_r1_text(r), since(t0)))
        return False
    p("CSD = %s  (%d ms)" % (hx(csd), since(t0)))
    if csd[0] >> 6 != 1:
        p("CSD v1 card (unsupported by this probe)")
        return False
    csize = ((csd[7] & 0x3F) << 16) | (csd[8] << 8) | csd[9]
    mib = (csize + 1) // 2
    p("capacity ~ %d MiB (%d MB)" % (mib, mib * 1024 * 1024 // 1000000))

    t0 = tick()
    mbr, r = card.read_lba(0)
    if mbr is None:
        p("RESULT: FAIL - CMD17 LBA0 R1=%s, %d ms" % (_r1_text(r), since(t0)))
        return
    p("LBA0  = %s" % hx(mbr, 16))
    p("LBA0 sig = %s  (%d ms)" % (hx(mbr[510:512]), since(t0)))
    if mbr[510] != 0x55 or mbr[511] != 0xAA:
        p("no 0x55AA at LBA0 (card not formatted, or wrong data path)")
        return False
    ptype = mbr[0x1C2]
    pstart = (mbr[0x1C6] | mbr[0x1C7] << 8 | mbr[0x1C8] << 16 | mbr[0x1C9] << 24)
    psize = (mbr[0x1CA] | mbr[0x1CB] << 8 | mbr[0x1CC] << 16 | mbr[0x1CD] << 24)
    p("part[0]: type=0x%02X start=%d size=%d" % (ptype, pstart, psize))

    p("reading partition boot sector at LBA %d (may take a moment)..." % pstart)
    t0 = tick()
    boot, r = card.read_lba(pstart)
    if boot is None:
        p("RESULT: FAIL - CMD17 LBA%d R1=%s, %d ms"
          % (pstart, _r1_text(r), since(t0)))
        return False
    p("boot sector read OK (%d ms), oem=%s" % (since(t0), hx(boot[3:11])))
    g = parse_fat32(boot)
    if g is None:
        p("RESULT: partition boot sector is not FAT32 (oem=%s)" % hx(boot[3:11]))
        return False
    p("FAT32: spc=%d rsvd=%d nfat=%d fatsz=%d root=%d total=%d"
      % (g["spc"], g["rsvd"], g["nfat"], g["fatsz"], g["root"], g["total"]))

    root_lba = pstart + g["root_lba"]
    p("reading root dir at LBA %d..." % root_lba)
    t0 = tick()
    root, r = card.read_lba(root_lba)
    if root is None:
        p("RESULT: FAIL - CMD17 LBA%d R1=%s, %d ms"
          % (root_lba, _r1_text(r), since(t0)))
        return False
    p("root dir read OK (%d ms)" % since(t0))
    for line in fat_list(root):
        p("  " + line)
    p("RESULT: PASS - card works as a SPI block device (mount layer not needed)")
    return True


def check():
    """main() with the console muted -> True when the card is usable.

    Caller (wait()) owns release(): this function leaves the SPI object open
    exactly like main() does.
    """
    global QUIET
    was, QUIET = QUIET, True
    try:
        return main() is True
    except Exception:
        return False
    finally:
        QUIET = was


def wait(interval_ms=2000, max_tries=0):
    """Block until the card is usable, then return True (False only if capped).

    This is the boot pre-flight the launcher runs before importing park_app, so
    that park_app can assume a sane card instead of carrying its own retry loop
    (2026-09-16, user).  Every attempt closes its SPI object first, so the app
    starts from a clean peripheral; max_tries=0 means "keep waiting forever",
    which is safe here because nothing else has been initialised yet - and a hot
    insert is picked up on the next attempt.
    """
    try:
        import utime
    except Exception:
        utime = None
    n = 0
    while True:
        ok = check()
        release()
        if ok:
            if n:
                print("[BOOT] sd_probe2: card up after %d retry(ies)" % n)
            return True
        n += 1
        if max_tries and n >= max_tries:
            print("[BOOT] sd_probe2: giving up after %d tries" % n)
            return False
        print("[BOOT] sd_probe2: no usable card (try %d) - retry in %d ms;"
              " park_app is NOT started" % (n, interval_ms))
        if utime is not None:
            utime.sleep_ms(interval_ms)


# ---- standalone run (saved as /flash/main.py).  Importing this module must NOT
# trigger it, or the launcher's "import sd_probe2" would print the whole report.
if __name__ != "sd_probe2":
    try:
        main()
    except Exception as e:
        # Never die silently in the IDE: the traceback may not reach the console
        # when the script is run from the editor.
        p("EXCEPTION: %r" % (e,))
print("SD2:==== probe finished ====")
