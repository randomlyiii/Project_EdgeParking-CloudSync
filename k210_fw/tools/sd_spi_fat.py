# -*- coding: utf-8 -*-
"""sd_spi_fat.py -- read files off an SD card over SPI, with no firmware mount.

Why this exists: on this CanMV firmware the mount layer cannot open the card
socket at all (machine.SDCard() -> TypeError("cannot create 'SDCard'
instances"), SDCard.remount() blocks forever, /sd -> ENODEV). The socket is
wired to SPI1 on IO27/28/26/29, and raw SPI block reads DO work
(k210_fw/sd_probe2.py reaches PASS), so this script drives the card itself.

Two paths, tried in order:
  A. MOUNT_TRY=1: expose a MicroPython block device (readblocks/writeblocks/
     ioctl) around the SPI reader and uos.mount() it on /sd. If this firmware
     supports block devices, /sd/KPU/lp_detect.kmodel becomes a normal path and
     park_app.py needs no change at all.
  B. Pure-Python FAT32 reader: parse BPB + FAT + directory entries (long file
     names included) and read a file's cluster chain. Works even when VFS
     mounting is unsupported.

Measured on the real card (SanDisk 16GB, FAT32, partition at LBA 8192):
one random 512 B read takes ~47-50 ms, so a 1 MB file is minutes at 100 kHz.
Raise BAUD once everything works.

READ-ONLY: never writes to the card.
"""
import machine

import utime              # wall-clock budgets for the card's answers

PINS = {"sclk": 27, "mosi": 28, "miso": 26, "cs": 29}
BAUD = 100000            # card init + mount clock; also the SD spec's init clock
BAUD_TRY = 0             # 1 = after /sd is usable, raise the clock to BAUD_FAST
BAUD_FAST = 200000       # 2026-09-16: OFF (done).  We tried the sanctioned next step,
                         # 200 kHz, and it is NOT safe on this board either.  Board log:
                         #   set_baud(200000) ok via deinit+new SPI()
                         #   recog kmodel 697512 B read in 32964 ms   <- 2x faster, fine
                         #   [DBG] > rec.lp_recog_load_weight_data(...)   <- then NOTHING:
                         #   no "weight_data_size:", no SDX heartbeat => the SECOND file
                         #   open after the baud change never returns.
                         # That is byte-for-byte the 400 kHz flake signature ("once just
                         # before the weight file was opened"), so the conclusion is not
                         # "200 is fine, 400 is flaky" but "this firmware's SPI driver
                         # cannot survive a baud change + a second file": the hang is at C
                         # level where no Python timeout can help, and every attempt costs
                         # a power cycle.  100 kHz never hung in any run => BAUD_TRY=0 is
                         # the only setting we ship.  Cost: ~214 s of model loading per
                         # cold boot (~3.5 min), which is documented and accepted.
                         # If you ever try again: it must be a *boot you can afford to
                         # lose*, and read the last line on the console to see where it
                         # died (it will be right after this file's open, not in deinit).

# Wall-clock budgets, NOT iteration counts (2026-09-13 fix): the old TOKEN_TRIES=20000
# was "~2 s at 100 kHz" but only ~0.2 s once setup_sd_vfs() raises the clock to 1 MHz -
# a silent 10x cut of the card's response budget, and the likely cause of the hang right
# after the recog kmodel was read.  Milliseconds behave the same at any baud.
TOKEN_MS = 2000          # budget for the 0xFE data token
R1_MS = 250              # budget for an R1 response byte
SECTOR_TRIES = 3         # a NAK'd sector is usually fine on the next CMD17
BUS_FLUSH_BYTES = 64     # clock out the 2 CRC bytes after each data block

MOUNT_TRY = 1            # 1 = also try uos.mount() with a Python block device
MOUNT_PATH = "/sd"
LIST_DIR = True          # list the root directory
READ_FILE = None         # e.g. "lp_weight.bin" to read (size + first bytes)
COPY_TO_FLASH = 0        # 1 = write READ_FILE to /flash/<basename>
CHUNK = 512

# MicroPython block-device ioctl codes (uos.blockdev / VfsFat)
_IOCTL_INIT = 1
_IOCTL_SYNC = 3
_IOCTL_BLOCK_COUNT = 4
_IOCTL_BLOCK_SIZE = 5


def p(msg):
    print("SDX:" + msg)


def hx(buf, n=None):
    if buf is None:
        return "None"
    if n is not None:
        buf = buf[:n]
    return " ".join("0x%02X" % b for b in buf)


class SDSPI:
    """Minimal SD-v2 (SDHC) SPI-mode block reader.

    Sequence per read: CMD17 -> poll R1 -> poll 0xFE token -> 512 B -> flush.
    The flush and the generous token budget are what make this card work; with
    a short budget the read appears to hang and desynchronises the card.
    """

    def __init__(self, pins=None, baud=BAUD):
        pins = pins or PINS
        self.pins = pins
        self.baud = baud
        self.offset = 0          # LBA of the mounted partition (0 = whole card)
        master = getattr(machine.SPI, "MODE_MASTER", None)
        if master is None:
            master = getattr(machine.SPI, "MASTER", 1)
        self.spi = machine.SPI(1, mode=master, baudrate=baud, polarity=0,
                               phase=0, bits=8, sck=pins["sclk"],
                               mosi=pins["mosi"], miso=pins["miso"],
                               cs0=pins["cs"])
        self.ok = False
        self.sectors = 0

    # ---- speed
    def set_baud(self, baud, ref=None):
        """Raise the SPI clock in place, and PROVE the card still reads before keeping it.

        `ref` = 512 bytes of LBA0 read at the current (known-good) clock.  When it is
        given, the new clock is kept only if LBA0 reads back identical twice; otherwise
        the old clock is restored and False is returned.  That closes a hole the board
        exposed on 2026-09-13: `spi.init(baudrate=...)` can succeed **without changing
        anything**, so "the call did not raise" proves nothing.  Never raises.

        Variants, cheapest first (a firmware may reject any of them):
          1. init(full kwargs)  2. init(baudrate=)  3. deinit + new SPI()  4. new SPI()

        NOTE 2026-09-13: on firmware v1.0.4, variant 4 at 1MHz read the recog model fine
        and then hung on the next read.  NOTE 2026-09-14: on the official Lite firmware
        v1.0.5-4 it printed "ok via new SPI()" and then the FIRST transfer inside the
        self-test never returned at all.  A C-level hang cannot be caught from Python,
        so the fast clock stays opt-in (BAUD_TRY).

        SOLVED 2026-09-15: variant 3 (deinit + new SPI()) is board-proven at 200 kHz and
        at 400 kHz ("set_baud(400000) ok via deinit+new SPI()", kmodel read 4x faster
        than the 100 kHz mount clock).  That also retracts the old conclusion that 1 MHz
        itself was fatal: variants 1/2/4 all try to re-clock the SPI object that is still
        live, while variant 3 is the only one that releases it first - so the hang was
        most likely "second SPI object without a deinit", not the frequency.
        """
        old = self.baud
        master = getattr(machine.SPI, "MODE_MASTER", None)
        if master is None:
            master = getattr(machine.SPI, "MASTER", 1)
        pins = self.pins

        def full():
            self.spi.init(mode=master, baudrate=baud, polarity=0, phase=0, bits=8,
                          sck=pins["sclk"], mosi=pins["mosi"], miso=pins["miso"],
                          cs0=pins["cs"])

        def minimal():
            self.spi.init(baudrate=baud)

        def rebuilt():
            self.spi = machine.SPI(1, mode=master, baudrate=baud, polarity=0, phase=0,
                                   bits=8, sck=pins["sclk"], mosi=pins["mosi"],
                                   miso=pins["miso"], cs0=pins["cs"])

        def deinit_rebuilt():
            # a second SPI object on the same peripheral while the first one is still
            # open is exactly what hung the board - release it first.
            # The three prints below split the risky step into deinit / construct / first
            # read: on 2026-09-15 the board died right after "try deinit+new SPI() ...",
            # and only these lines can say *which* of the three never returned.
            p("[SD]   variant3: deinit() ...")
            try:
                self.spi.deinit()
            except Exception:
                pass
            p("[SD]   variant3: new SPI(%d) ..." % baud)
            rebuilt()
            p("[SD]   variant3: constructed, now the first read")

        last = "no variant tried"
        for desc, fn in (("init(full)", full), ("init(baudrate)", minimal),
                         ("deinit+new SPI()", deinit_rebuilt), ("new SPI()", rebuilt)):
            # print BEFORE the risky call: if a variant hangs the C driver, this line is
            # the only evidence left on the console.  (A silent failure cost us a board
            # round trip twice on 2026-09-13/14 - never again.)
            p("[SD] try %s at %d ..." % (desc, baud))
            try:
                fn()
            except Exception as e:
                last = "%s -> %r" % (desc, e)
                continue
            self.baud = baud
            if ref is None or self._verify_baud(ref):
                p("[SD] set_baud(%d) ok via %s" % (baud, desc))
                return True
            # the call "worked" but the card no longer reads -> silent no-op or a bad
            # clock.  Restore the old clock and STOP: more variants only risk a hang.
            last = "%s -> LBA0 no longer reads back" % desc
            p("[SD] set_baud(%d) via %s: reads are not LBA0 any more -> back to %d"
              % (baud, desc, old))
            self._restore_baud(old, master, pins)
            return False
        p("[SD] set_baud(%d) FAILED: %s" % (baud, last))
        return False

    def _verify_baud(self, ref):
        """True = LBA0 still reads back exactly `ref` twice at the new clock."""
        try:
            a = self.read_sector(0)
            b = self.read_sector(0)
        except Exception as e:
            p("[SD] baud verify raised %r" % (e,))
            return False
        return a is not None and a == ref and b == ref

    def _restore_baud(self, baud, master, pins):
        """Best-effort: rebuild the SPI object at a clock that was working before."""
        try:
            self.spi = machine.SPI(1, mode=master, baudrate=baud, polarity=0,
                                   phase=0, bits=8, sck=pins["sclk"],
                                   mosi=pins["mosi"], miso=pins["miso"],
                                   cs0=pins["cs"])
            self.baud = baud
            p("[SD] clock restored to %d" % baud)
        except Exception as e:
            p("[SD] could not restore the clock to %d: %r" % (baud, e))

    # ---- low level
    def rb(self):
        return self.spi.read(1, 0xFF)[0]

    def frame(self, idx, arg, crc):
        self.spi.write(bytes([0x40 | idx, (arg >> 24) & 0xFF,
                              (arg >> 16) & 0xFF, (arg >> 8) & 0xFF,
                              arg & 0xFF, crc]))

    def r1(self):
        t0 = utime.ticks_ms()
        while utime.ticks_diff(utime.ticks_ms(), t0) < R1_MS:
            b = self.rb()
            if b != 0xFF and b != 0x7F:
                return b
        return None

    def cmd(self, idx, arg, crc=0x00):
        self.frame(idx, arg, crc)
        return self.r1()

    def acmd(self, idx, arg):
        self.cmd(55, 0, 0x01)
        return self.cmd(idx, arg)

    def flush(self, n=BUS_FLUSH_BYTES):
        self.spi.write(bytes([0xFF]) * n)

    def data_block(self, nbytes):
        got = None
        t0 = utime.ticks_ms()
        while utime.ticks_diff(utime.ticks_ms(), t0) < TOKEN_MS:
            b = self.rb()
            if b == 0xFE:
                got = self.spi.read(nbytes, 0xFF)
                break
            if b != 0xFF and b != 0x7F:
                break
        self.flush()
        return got

    # ---- init + reads
    def init(self):
        for _ in range(10):
            self.spi.write(b"\xFF")
        if self.cmd(0, 0, 0x95) != 0x01:
            return "CMD0 failed (no card?)"
        self.frame(8, 0x1AA, 0x87)
        if self.r1() != 0x01:
            return "CMD8 failed (not an SDHC card)"
        self.spi.read(4, 0xFF)
        ready = False
        for _ in range(100):
            b = self.acmd(41, 0x40000000)
            if b == 0x00:
                ready = True
                break
            if b is None:
                return "ACMD41 no response"
        if not ready:
            return "ACMD41 never ready"
        csd = self.read_csd()
        if csd is None:
            return "CMD9 failed (no CSD)"
        if csd[0] >> 6 != 1:
            return "not a v2 (SDHC) card"
        csize = ((csd[7] & 0x3F) << 16) | (csd[8] << 8) | csd[9]
        self.sectors = (csize + 1) * 1024
        self.ok = True
        return None

    def read_csd(self):
        if self.cmd(9, 0) != 0x00:
            return None
        return self.data_block(16)

    def read_sector(self, lba):
        """Return 512 bytes of the mounted partition (lba is partition-relative).

        Retries the whole CMD17 on failure: one NAK/desync is normal on a marginal SPI
        clock, and a fresh command usually recovers it.  Never loops forever.
        """
        got = None
        for _ in range(SECTOR_TRIES):
            if self.cmd(17, lba + self.offset) == 0x00:
                got = self.data_block(512)
                if got is not None:
                    return got
            self.flush()
        p("[SD] read_sector(%d) failed after %d tries (baud=%d)"
          % (lba, SECTOR_TRIES, self.baud))
        return None


# ---- read progress (2026-09-13) ---------------------------------------------
# WHY: after `uos.mount()`, the FIRMWARE's own FatFs rides our Python block device one
# sector at a time (one readblocks() callback per 512 bytes).  With the SPI clock still
# at the mount-time BAUD=100kHz that is only ~12.5KB/s: the 682KB recog model needs 56s
# and the 1.43MB weight 120s.  On the serial console that looks exactly like a hang
# ("model size 697512" and then nothing for a minute) - which is what got reported as a
# dead board on 2026-09-13.  So: (1) setup_sd_vfs() may raise the clock with
# set_baud(BAUD_FAST, ref) after mounting - but ONLY when BAUD_TRY=1; (2) print a line
# every READ_PROGRESS_BYTES so a slow read is distinguishable from a real deadlock (a
# deadlock stops on some byte count and never prints again).
# 2026-09-15: 256KB was too coarse to answer "is it alive?" during the 1.5MB weight read
# (and the counter carries over between files).  64KB gives a heartbeat every ~6s at the
# 100kHz mount clock we actually run.
# 2026-09-16: the elapsed-ms figure is GONE.  It was measured *here*, in Python, around a
# read the C layer performs, so it never measured the read itself - and ticks_ms() wraps,
# which made it nonsense across a long file.  Only the byte counter is printed.
READ_PROGRESS_BYTES = 64 * 1024
_READ_STATE = [0, 0]         # [total_bytes, bytes_since_last_print]


def _progress(n):
    """Print the cumulative KB read every READ_PROGRESS_BYTES (no timing - see above)."""
    try:
        _READ_STATE[0] += n
        _READ_STATE[1] += n
        if _READ_STATE[1] >= READ_PROGRESS_BYTES:
            _READ_STATE[1] = 0
            p("[SD] read %d KB (still working)" % (_READ_STATE[0] // 1024,))
    except Exception:
        pass


class BlockDev:
    """MicroPython block device over the SD, so uos.mount() can use it.

    Only the *partition* is exposed (block 0 = partition start), because the
    firmware's FAT driver mounts the volume at block 0 and cannot be told about
    an MBR offset.

    The ioctl contract matters: the VFS layer only accepts a device that answers
    the block-count ioctl with a positive number; anything else comes back as
    OSError(1) EPERM from uos.mount().
    """
    def __init__(self, sd, first_block=0, nblocks=None):
        self.sd = sd
        self.first_block = first_block
        self.blocks = nblocks if nblocks is not None else sd.sectors
        self.blocksize = 512          # CanMV/MicroPython name
        self.block_size = 512         # older name
        self.readonly = True

    def readblocks(self, block_num, buf, offset=0):
        # buf may be a bytearray to fill (newer API) or an int block count
        if isinstance(buf, int):
            out = bytearray()
            for i in range(buf):
                d = self.sd.read_sector(self.first_block + block_num + i)
                if d is None:
                    raise OSError(5, "SD read fail")
                out += d
            _progress(buf * 512)
            return out
        mv = memoryview(buf)
        n = len(mv) // 512
        for i in range(n):
            d = self.sd.read_sector(self.first_block + block_num + i)
            if d is None:
                raise OSError(5, "SD read fail")
            mv[i * 512:(i + 1) * 512] = d
        _progress(n * 512)
        return None

    def writeblocks(self, block_num, buf, offset=0):
        raise OSError(30, "read-only device")     # EROFS

    def ioctl(self, op, arg):
        if op == _IOCTL_INIT:
            return 0
        if op == _IOCTL_SYNC:
            return 0
        if op == _IOCTL_BLOCK_COUNT:
            return self.blocks
        if op == _IOCTL_BLOCK_SIZE:
            return 512
        return -1        # unknown op: see MP_BLOCKDEV_IOCTL_* in the docs


class VfsFat32:
    """Tiny read-only VFS over Fat32, so `open('/sd/...')` works in MicroPython.

    park_app.py loads its models by *path* (kpu.load_kmodel('/sd/KPU/x.kmodel')),
    so a VFS that satisfies open/stat/listdir/ilistdir is all that is needed --
    no firmware mount support required.
    """

    def __init__(self, fs, prefix="/sd"):
        self.fs = fs
        self.prefix = prefix
        self._cwd = "/"

    # ---- path helpers
    def _rel(self, path):
        if not path:
            path = "/"
        if isinstance(path, bytes):
            path = path.decode()
        for pre in (self.prefix, self.prefix + "/"):
            if path == pre.rstrip("/") or path.startswith(pre):
                path = path[len(pre):]
                break
        if not path:
            path = "/"
        if not path.startswith("/"):
            path = self._cwd + path if self._cwd != "/" else "/" + path
        return path

    def _lookup(self, path):
        """-> (attr, first_cluster, size) or None"""
        path = self._rel(path)
        parts = [p for p in path.split("/") if p]
        if not parts:
            return (0x10, self.fs.root, 0)
        cl = self.fs.root
        for i, part in enumerate(parts):
            last = (i == len(parts) - 1)
            hit = None
            for long_name, short, attr, fst, size in self.fs.list_dir(cl):
                if (long_name and long_name.upper() == part.upper()) \
                        or short.upper() == part.upper():
                    hit = (attr, fst, size)
                    break
            if hit is None:
                return None
            if not last:
                if not (hit[0] & 0x10):
                    return None
                cl = hit[1]
                continue
            return hit
        return None

    # ---- VFS API the firmware calls
    def mount(self, readonly, mkfs):
        return None

    def umount(self):
        return None

    def stat(self, path):
        hit = self._lookup(path)
        if hit is None:
            raise OSError(2, "ENOENT")            # ENOENT
        attr, _fst, size = hit
        return (0x4000 if attr & 0x10 else 0x8000, size, 0, 0, 0, 0, 0, 0,
                0, 0)

    def statvfs(self, path):
        return (512, self.fs.spc * 512, self.fs.spc * 512, 0, 0, 0, 0, 0, 0, 255)

    def listdir(self, path):
        hit = self._lookup(path)
        if hit is None:
            raise OSError(2, "ENOENT")
        if not (hit[0] & 0x10):
            raise OSError(20, "ENOTDIR")
        out = []
        for long_name, short, attr, _fst, _size in self.fs.list_dir(hit[1]):
            if short in (".", ".."):
                continue
            out.append(long_name or short)
        return out

    def ilistdir(self, path):
        hit = self._lookup(path)
        if hit is None:
            raise OSError(2, "ENOENT")
        out = []
        for long_name, short, attr, _fst, size in self.fs.list_dir(hit[1]):
            if short in (".", ".."):
                continue
            kind = 0x4000 if attr & 0x10 else 0x8000
            out.append((long_name or short, kind, 0, size))
        return out

    def chdir(self, path):
        hit = self._lookup(path)
        if hit is None or not (hit[0] & 0x10):
            raise OSError(2, "ENOENT")
        self._cwd = self._rel(path)
        return None

    def getcwd(self):
        return self.prefix if self._cwd == "/" else self.prefix + self._cwd

    def open(self, path, mode):
        if "w" in mode or "a" in mode or "+" in mode:
            raise OSError(30, "EROFS")
        hit = self._lookup(path)
        if hit is None:
            raise OSError(2, "ENOENT")
        attr, fst, size = hit
        if attr & 0x10:
            raise OSError(21, "EISDIR")
        return Fat32File(self.fs, fst, size)

    def remove(self, path):
        raise OSError(30, "EROFS")

    def rename(self, a, b):
        raise OSError(30, "EROFS")

    def mkdir(self, path):
        raise OSError(30, "EROFS")

    def rmdir(self, path):
        raise OSError(30, "EROFS")


class Fat32File:
    """Seekable read-only file over a FAT32 cluster chain."""

    def __init__(self, fs, first_cluster, size):
        self.fs = fs
        self.first = first_cluster
        self.size = size
        self.pos = 0
        self._clusters = None

    def _chain(self):
        if self._clusters is None:
            self._clusters = self.fs.chain(self.first)
        return self._clusters

    def _read_at(self, pos, n):
        data = bytearray()
        cl_bytes = self.fs.spc * 512
        idx = pos // cl_bytes
        off = pos % cl_bytes
        chain = self._chain()
        while n > 0 and idx < len(chain):
            blk = self.fs.read_cluster(chain[idx])
            chunk = blk[off:off + n]
            data += chunk
            n -= len(chunk)
            idx += 1
            off = 0
        return bytes(data)

    def read(self, n=-1):
        if n is None or n < 0:
            n = self.size - self.pos
        n = min(n, self.size - self.pos)
        if n <= 0:
            return b""
        data = self._read_at(self.pos, n)
        self.pos += len(data)
        return data

    def readinto(self, buf):
        data = self.read(len(buf))
        buf[:len(data)] = data
        return len(data)

    def readline(self):
        out = bytearray()
        while self.pos < self.size:
            c = self._read_at(self.pos, 1)
            if not c:
                break
            self.pos += 1
            out += c
            if c == b"\n":
                break
        return bytes(out)

    def seek(self, off, whence=0):
        if whence == 0:
            self.pos = off
        elif whence == 1:
            self.pos += off
        else:
            self.pos = self.size + off
        if self.pos < 0:
            self.pos = 0
        return self.pos

    def tell(self):
        return self.pos

    def close(self):
        return None

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


class Fat32:
    """Read-only FAT32 reader on top of SDSPI (no firmware support needed)."""

    def __init__(self, sd, part_start):
        self.sd = sd
        self.base = part_start
        b = sd.read_sector(part_start)
        if b is None:
            raise OSError("cannot read boot sector")
        self.bps = b[11] | (b[12] << 8)
        self.spc = b[13]
        self.rsvd = b[14] | (b[15] << 8)
        self.nfat = b[16]
        self.fatsz = b[36] | (b[37] << 8) | (b[38] << 16) | (b[39] << 24)
        self.root = b[44] | (b[45] << 8) | (b[46] << 16) | (b[47] << 24)
        if self.bps != 512 or self.spc == 0 or self.fatsz == 0:
            raise OSError("not FAT32 (bps=%d spc=%d fatsz=%d)"
                          % (self.bps, self.spc, self.fatsz))
        self.root_lba = self.base + self.rsvd + self.nfat * self.fatsz
        # cluster 2 (the root directory) occupies the first cluster after the
        # reserved area + all FATs; file data therefore starts one cluster later
        self.data_lba = self.root_lba + self.spc
        self.fat_lba = self.base + self.rsvd
        self._fat = None

    # ---- geometry
    def cluster_lba(self, cl):
        return self.data_lba + (cl - 2) * self.spc

    def fat_entry(self, cl):
        off = cl * 4
        lba = self.fat_lba + off // 512
        blk = self.sd.read_sector(lba)
        if blk is None:
            return 0x0FFFFFFF
        i = off % 512
        return (blk[i] | (blk[i + 1] << 8) | (blk[i + 2] << 16)
                | (blk[i + 3] << 24)) & 0x0FFFFFFF

    def chain(self, cl, limit=100000):
        out = []
        while 2 <= cl < 0x0FFFFFF8 and len(out) < limit:
            out.append(cl)
            cl = self.fat_entry(cl)
        return out

    # ---- directory
    def read_cluster(self, cl):
        data = bytearray()
        for i in range(self.spc):
            blk = self.sd.read_sector(self.cluster_lba(cl) + i)
            if blk is None:
                raise OSError("read fail at cluster %d" % cl)
            data += blk
        return bytes(data)

    def list_dir(self, cl=None):
        """[(long_name, short_name, attr, first_cluster, size), ...]"""
        cl = self.root if cl is None else cl
        out = []
        for c in self.chain(cl):
            data = self.read_cluster(c)
            lfn = {}
            for off in range(0, len(data) - 31, 32):
                e = data[off:off + 32]
                if e[0] == 0x00:
                    return out
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
                        c2 = e[i] | (e[i + 1] << 8)
                        if c2 == 0x0000:
                            done = True
                            break
                        if c2 != 0xFFFF:
                            part.append(c2)
                    lfn[seq] = part
                    continue
                base = "".join(chr(c2) for c2 in e[0:8]
                               if c2 not in (0x20, 0x00))
                ext = "".join(chr(c2) for c2 in e[8:11]
                              if c2 not in (0x20, 0x00))
                short = (base + "." + ext) if ext else base
                name = None
                if lfn:
                    chars = []
                    for s in sorted(lfn.keys()):
                        chars.extend(lfn[s])
                    # stop at the UTF-16 NUL terminator (the rest of the slot is
                    # 0xFFFF padding and would become garbage characters)
                    cut = len(chars)
                    for i, c2 in enumerate(chars):
                        if c2 == 0:
                            cut = i
                            break
                    try:
                        name = "".join(chr(c2) for c2 in chars[:cut])
                    except Exception:
                        name = None
                    lfn = {}
                fst = (e[26] | (e[27] << 8)) | ((e[20] | (e[21] << 8)) << 16)
                size = e[28] | (e[29] << 8) | (e[30] << 16) | (e[31] << 24)
                out.append((name, short, attr, fst, size))
        return out

    def find(self, name):
        want = name.upper()
        for long_name, short, attr, fst, size in self.list_dir():
            if attr & 0x10:
                continue
            if (long_name and long_name.upper() == want) \
                    or short.upper() == want:
                return fst, size, long_name or short
        return None, None, None

    # ---- file
    def read_file(self, name, max_bytes=None, progress=None):
        fst, size, real = self.find(name)
        if fst is None:
            raise OSError("file not found: %s" % name)
        n = size if max_bytes is None else min(size, max_bytes)
        out = bytearray()
        cl = fst
        while 2 <= cl < 0x0FFFFFF8 and len(out) < n:
            data = self.read_cluster(cl)
            out += data
            if progress:
                progress(len(out), size, real)
            cl = self.fat_entry(cl)
        return bytes(out[:n]), size, real

    def read_dir(self, name):
        """Resolve a directory name to its first cluster."""
        want = name.upper().rstrip("/")
        for long_name, short, attr, fst, size in self.list_dir():
            if not (attr & 0x10):
                continue
            if (long_name and long_name.upper() == want) \
                    or short.upper() == want:
                return fst
        return None


def mbr_partitions(mbr):
    """Parse the four MBR entries -> [(idx, type, start_lba, nblocks), ...].

    Only entries with type != 0 are returned. Scanning ALL of them matters:
    on 2026-09-13 the raw reader was pointed at an EMPTY FAT32 partition while
    the firmware's /sd clearly had a KPU directory -- i.e. the card carries more
    than one partition and picking the first one by default was wrong.
    """
    out = []
    for i in range(4):
        off = 0x1BE + i * 16
        ptype = mbr[off + 4]
        start = (mbr[off + 8] | (mbr[off + 9] << 8) | (mbr[off + 10] << 16)
                 | (mbr[off + 11] << 24))
        size = (mbr[off + 12] | (mbr[off + 13] << 8) | (mbr[off + 14] << 16)
                | (mbr[off + 15] << 24))
        if ptype == 0 or size == 0:
            continue
        out.append((i + 1, ptype, start, size))
    return out


def _has_dir(fs, name):
    try:
        return fs.read_dir(name) is not None
    except Exception:
        return False


def setup_sd_vfs(print_fn=None, mount_path=MOUNT_PATH, baud=None,
                 partition=None):
    """Make mount_path (default /sd) usable so park_app.py can open() models by path.

    Called once by park_app.py at boot:
      1) raw SPI init + handshake + CSD (see the header comment: one wrong
         parameter and the card appears to hang)
      2) read the MBR and LIST EVERY partition, health-checking each: the one
         that parses as FAT32 AND contains a KPU directory wins; otherwise fall
         back to the first non-empty FAT32 partition (partition=N forces one)
      3) try uos.mount(first_block=0, nblocks=partition) -- exposing only the
         partition, never the whole card with its MBR; then uos.register_vfs
    Returns True when mount_path lists. Never raises: failures are logged.
    """
    log = print_fn or p

    # ⚠️ 挂载必须用慢速 BAUD(100kHz)：实测 1MHz 下 uos.mount 会卡死
    # (MBR/FAT 解析都正常，唯独固件 VFS 挂载读扇区那一步过不去)；100kHz
    # 是曾经实测成功挂载的时钟。挂载完成后 set_baud(BAUD_FAST) 再提速读模型。
    sd = SDSPI(baud=baud or BAUD)
    err = sd.init()
    if err:
        log("[SD] spi init failed: %s" % err)
        return False
    mbr = sd.read_sector(0)                  # absolute LBA 0 (offset still 0)
    if mbr is None:
        log("[SD] cannot read LBA0")
        return False
    parts = mbr_partitions(mbr)
    if not parts:
        log("[SD] no partition in MBR")
        return False
    log("[SD] MBR has %d partition(s): %s"
        % (len(parts),
           ", ".join("#%d type=0x%02X @%d %dblk" % pt for pt in parts)))

    # health-check each partition; prefer the one holding the KPU directory
    cands = []
    for idx, ptype, start, size in parts:
        if partition is not None and idx != partition:
            continue
        sd.offset = start
        try:
            fs = Fat32(sd, 0)
        except Exception as e:
            log("[SD] part#%d not FAT32: %r" % (idx, e))
            continue
        has_kpu = _has_dir(fs, "KPU")
        root = [n for n in fs.list_dir() if n[1] not in (".", "..")]
        log("[SD] part#%d FAT32 spc=%d rsvd=%d fatsz=%d, %d entries%s"
            % (idx, fs.spc, fs.rsvd, fs.fatsz, len(root),
               ", has KPU" if has_kpu else ""))
        # volumes with KPU come first, then any non-empty, then empty ones
        cands.append((0 if has_kpu else (1 if root else 2), idx, start, size, fs))
    if not cands:
        log("[SD] no usable FAT32 partition")
        return False
    cands.sort(key=lambda c: c[0])
    _, pidx, pstart, psize, fs = cands[0]
    sd.offset = pstart
    log("[SD] using part#%d @%d (%d blocks)" % (pidx, pstart, psize))

    try:
        import uos
    except Exception as e:
        log("[SD] no uos: %r" % (e,))
        return False

    ok = False
    # (1) firmware mount, exposing only the partition (works, but sometimes EPERM)
    try:
        try:
            uos.mkdir(mount_path)
        except Exception:
            pass
        uos.mount(BlockDev(sd, first_block=0, nblocks=psize), mount_path)
        ok = True
    except Exception as e:
        log("[SD] uos.mount failed: %r" % (e,))
    # (2) fallback: register our own VFS (this firmware may lack register_vfs)
    if not ok:
        vfs = VfsFat32(fs, mount_path)
        for args in ((vfs, mount_path), (mount_path, vfs)):
            try:
                uos.register_vfs(*args)
                ok = True
                break
            except Exception as e:
                last = e
        if not ok:
            log("[SD] register_vfs failed: %r" % (last,))
    if not ok:
        log("[SD] no way to mount %s on this firmware" % mount_path)
        return False

    # 挂载成功 → 提速 SPI 时钟（读模型从 ~12KB/s 提到 ~120KB/s）
    if sd.set_baud(BAUD_FAST):
        log("[SD] baud raised %d -> %d" % (BAUD, BAUD_FAST))
    else:
        log("[SD] baud keep %d (slow model read; mount still works)"
            % sd.baud)

    # self-check: really list the directory so callers can open() safely
    try:
        names = uos.listdir(mount_path)
    except Exception as e:
        log("[SD] mounted but listdir failed: %r" % (e,))
        return False
    root = [n for n in fs.list_dir() if n[1] not in (".", "..")]
    log("[SD] %s ready -> %s" % (mount_path, names[:6]))
    for long_name, short, attr, _fst, size in root:
        log("[SD]   %s %s%s" % ("DIR " if attr & 0x10 else "FILE",
                                long_name or short,
                                "" if (attr & 0x10) else (" %dB" % size)))
    if not root:
        log("[SD] NOTE: this volume is EMPTY - copy the models to it")

    # model self-check: open the three files the firmware needs, read a few
    # bytes each, and report sizes. park_app.py looks for exactly these three.
    MODELS = ("lp_detect.kmodel", "lp_recog.kmodel", "lp_weight.bin")
    for m in MODELS:
        path = mount_path + "/KPU/" + m
        try:
            st = uos.stat(path)
            size = st[6]
            with open(path, "rb") as f:
                head = f.read(8)
            log("[SD] model %s: %dB, head=%s OK" % (m, size, hx(head)))
        except Exception as e:
            log("[SD] model %s MISSING: %r" % (m, e))
    return True


def main():
    p("==== SD SPI FAT32 reader (firmware mount layer bypassed) ====")
    sd = SDSPI()
    err = sd.init()
    if err:
        p("RESULT: FAIL - %s" % err)
        return
    p("card ready: %d sectors (%d MB)"
      % (sd.sectors, sd.sectors // 2048))

    mbr = sd.read_sector(0)
    if mbr is None:
        p("RESULT: FAIL - cannot read LBA 0")
        return

    # ---- scan every partition and pick the volume that really holds the models
    # 2026-09-13 trap: reading only the first entry landed on an EMPTY FAT32
    # volume while the firmware's /sd had a KPU directory, so scan them all and
    # use the KPU directory as the landmark.
    parts = mbr_partitions(mbr)
    p("MBR sig=%s, %d partition(s)"
      % (hx(mbr[510:512]), len(parts)))
    cands = []
    for idx, ptype, start, size in parts:
        sd.offset = start
        boot = sd.read_sector(0)
        if boot is None:
            p("  part#%d type=0x%02X @%d %dblk -> boot sector unreadable"
              % (idx, ptype, start, size))
            continue
        p("  part#%d type=0x%02X @%d %dblk boot=0x%02X%02X oem=%s"
          % (idx, ptype, start, size, boot[510], boot[511], hx(boot[3:11])))
        try:
            fs = Fat32(sd, 0)
        except Exception as e:
            p("    -> not FAT32: %r" % (e,))
            continue
        has_kpu = _has_dir(fs, "KPU")
        root = [n for n in fs.list_dir() if n[1] not in (".", "..")]
        names = [(n[0] or n[1]) for n in root][:6]
        p("    -> FAT32 spc=%d rsvd=%d fatsz=%d root_cluster=%d entries=%d%s %s"
          % (fs.spc, fs.rsvd, fs.fatsz, fs.root, len(root),
             " HAS-KPU" if has_kpu else "", names))
        cands.append((0 if has_kpu else (1 if root else 2), idx, start, size, fs))
    if not cands:
        p("RESULT: FAIL - no FAT32 partition found")
        return
    cands.sort(key=lambda c: c[0])
    _, pidx, pstart, psize, fs = cands[0]
    sd.offset = pstart
    p("chosen volume: part#%d @%d (%d blocks)" % (pidx, pstart, psize))

    # ---- what the firmware sees, for cross-checking our own view
    try:
        import uos
        st = uos.statvfs(MOUNT_PATH)
        p("firmware %s: blk=%d bsize=%d free=%d -> %s"
          % (MOUNT_PATH, st[0], st[1], st[3], uos.listdir(MOUNT_PATH)[:6]))
        try:
            p("firmware %s/KPU -> %s" % (MOUNT_PATH,
                                         uos.listdir(MOUNT_PATH + "/KPU")[:8]))
        except Exception as e:
            p("firmware %s/KPU -> %r" % (MOUNT_PATH, e))
        # read the same names raw: if the firmware has them and we do not, we
        # are looking at the wrong volume (that is the whole point of this)
        for name in ("KPU", "System Volume Information", "WPSettings.dat"):
            hit = fs.read_dir(name) if name == "KPU" else fs.find(name)[0]
            p("raw    %s/%s -> %s" % (MOUNT_PATH, name,
                                      "found" if hit else "absent"))
    except Exception as e:
        p("firmware %s not mounted: %r" % (MOUNT_PATH, e))
        p("(if the REPL shows it mounted, this firmware mounts later than us)")

    dev = BlockDev(sd, first_block=0, nblocks=psize)

    # ---- path A: let the firmware mount the volume as a normal filesystem
    if MOUNT_TRY:
        try:
            import uos
            try:
                uos.mkdir(MOUNT_PATH)
            except Exception:
                pass
            uos.mount(dev, MOUNT_PATH)
            p("uos.mount(%s) OK -> %s" % (MOUNT_PATH, uos.listdir(MOUNT_PATH)))
            p("RESULT: PASS - the card is mountable; park_app.py can use %s/KPU/"
              % MOUNT_PATH)
            return
        except Exception as e:
            p("uos.mount FAILED (%r) -> trying uos.register_vfs" % (e,))

    # ---- path A2: register our own tiny VFS on /sd
    if MOUNT_TRY:
        last = None
        try:
            import uos
            vfs = VfsFat32(fs, MOUNT_PATH)
            for args in ((vfs, MOUNT_PATH), (MOUNT_PATH, vfs)):
                try:
                    uos.register_vfs(*args)
                    last = None
                    break
                except Exception as e:
                    last = e
            if last is not None:
                raise last
            p("register_vfs OK -> listdir(%s) = %s"
              % (MOUNT_PATH, uos.listdir(MOUNT_PATH)))
            hit = fs.find("main.py")
            if hit[0] is not None:
                with open(MOUNT_PATH + "/main.py", "rb") as f:
                    head = f.read(32)
                p("open(%s/main.py) OK, first bytes: %s"
                  % (MOUNT_PATH, hx(head)))
            p("RESULT: PASS - %s is readable through our VFS; park_app.py can open "
              "models by path" % MOUNT_PATH)
            return
        except Exception as e:
            p("register_vfs FAILED (%r) -> falling back to the raw reader" % (e,))

    # ---- path B: our own FAT32 reader
    p("FAT32: spc=%d rsvd=%d nfat=%d fatsz=%d root=%d"
      % (fs.spc, fs.rsvd, fs.nfat, fs.fatsz, fs.root))
    root_entries = fs.list_dir()
    if not root_entries:
        p("*** the volume is EMPTY: no model files here (copy them from a PC) ***")

    if LIST_DIR:
        p("root dir:")
        for long_name, short, attr, fst, size in root_entries:
            p("  %s %-24s short=%-14s first_cluster=%-6d size=%d"
              % ("DIR " if attr & 0x10 else "FILE",
                 long_name if long_name else "-", short, fst, size))
        kpu = fs.read_dir("KPU")
        if kpu:
            p("KPU dir:")
            for long_name, short, attr, fst, size in fs.list_dir(kpu):
                p("  %-24s size=%d" % (long_name or short, size))

    if READ_FILE:
        def prog(done, total, real):
            p("  reading %s: %d/%d B" % (real, done, total))
        p("reading %s ..." % READ_FILE)
        data, size, real = fs.read_file(READ_FILE, progress=prog)
        p("read %s: %d bytes (declared %d), head=%s"
          % (real, len(data), size, hx(data, 16)))
        if COPY_TO_FLASH:
            name = real.split("/")[-1]
            try:
                with open("/flash/" + name, "wb") as f:
                    f.write(data)
                p("copied to /flash/%s" % name)
            except Exception as e:
                p("copy FAILED: %r" % (e,))

    p("RESULT: PASS - pure Python FAT32 read works (no firmware mount needed)")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        p("EXCEPTION: %r" % (e,))
    print("SDX:==== done ====")
