# -*- coding: utf-8 -*-
"""Host regression for k210_fw/sd_spi_fat.py: partition offset, block-device
ioctl contract, and the tiny VFS (open/read/seek/listdir/ilistdir/stat) that
lets main.py load models by path with no firmware mount support.
"""
import os
import struct
import sys
import types

REPO = r"D:\Projects\Project_EdgeParking-CloudSync"
BASE = os.path.join(REPO, "k210_fw")
SECTOR = 512

sys.path.insert(0, os.path.join(BASE, "tools"))
import test_sd_spi_fat as H          # reuses the synthetic-card builder

IMAGE = H.build_image()
PART_START = H.PART_START


def load(monkeypatch=None):
    src = open(os.path.join(BASE, "tools", "sd_spi_fat.py"), encoding="utf-8").read()
    cut = src.find('\nif __name__ == "__main__":')
    assert cut > 0, "run block not found"
    src = src[:cut]
    sys.modules["machine"] = H.make_machine(IMAGE)

    # minimal uos with the two functions main()/the VFS path may touch
    uos = types.ModuleType("uos")
    calls = []

    def mount(dev, path):
        calls.append(("mount", path))
        raise OSError(1)                  # this firmware refuses Python blockdevs

    def register_vfs(*args):
        calls.append(("register_vfs", args))

    uos.mount = mount
    uos.register_vfs = register_vfs
    uos.mkdir = lambda p: calls.append(("mkdir", p))
    uos.listdir = lambda p: ["main.py", "KPU"] if p == "/sd" else []
    sys.modules["uos"] = uos
    ns = {"__name__": "under_test"}
    exec(compile(src, "sd_spi_fat.py", "exec"), ns)
    ns["_calls"] = calls
    return ns


def build_fs(ns):
    sd = ns["SDSPI"]()
    err = sd.init()
    assert err is None, err
    sd.offset = PART_START                # partition-relative from here on
    return sd, ns["Fat32"](sd, 0)


def main():
    ns = load()
    sd, fs = build_fs(ns)

    print("== 1. partition offset ==")
    # sd.read_sector(0) must now be the partition's boot sector, not the MBR
    boot = sd.read_sector(0)
    assert boot[3:11] == b"MSWIN4.1", "offset not applied: %r" % boot[3:11]
    print("   read_sector(0) = partition boot sector (oem=%s)"
          % boot[3:11].decode())

    print("== 2. block-device ioctl contract ==")
    dev = ns["BlockDev"](sd, first_block=0, nblocks=H.PART_SIZE)
    assert dev.ioctl(4, 0) == H.PART_SIZE, dev.ioctl(4, 0)
    assert dev.ioctl(5, 0) == 512
    assert dev.ioctl(1, 0) == 0 and dev.ioctl(3, 0) == 0
    assert dev.ioctl(99, 0) == -1, "unknown ioctls must not report success"
    assert dev.readonly is True and dev.blocksize == 512
    try:
        dev.writeblocks(0, bytearray(512))
        raise AssertionError("writeblocks must refuse")
    except OSError as e:
        assert e.args[0] == 30, e            # EROFS
    buf = bytearray(1024)
    dev.readblocks(0, buf)
    assert buf[3:11] == b"MSWIN4.1"
    print("   ioctl(BLOCK_COUNT/BLOCK_SIZE/INIT/SYNC) ok, unknown -> -1, "
          "readblocks ok, writeblocks -> EROFS")

    print("== 3. VFS ==")
    vfs = ns["VfsFat32"](fs, "/sd")
    assert vfs.mount(True, False) is None
    listing = vfs.listdir("/sd")
    assert H.LONG_NAME in listing, listing
    assert "KPUDIR" in [x.upper() for x in listing], listing
    ilist = dict((e[0], (e[1], e[3])) for e in vfs.ilistdir("/sd"))
    assert ilist[H.LONG_NAME][0] == 0x8000, ilist
    assert ilist["KPUDIR"][0] == 0x4000, ilist
    kind, size = vfs.stat("/sd/" + H.LONG_NAME)[:2]
    assert kind == 0x8000 and size == len(H.FILE_DATA), (kind, size)
    print("   listdir/ilistdir/stat ok (long name preserved, dir flagged)")

    with vfs.open("/sd/" + H.LONG_NAME, "rb") as f:
        assert f.read(16) == H.FILE_DATA[:16]
        f.seek(H.CLUSTER_BYTES - 8)          # straddle a cluster boundary
        assert f.read(16) == H.FILE_DATA[H.CLUSTER_BYTES - 8:
                                         H.CLUSTER_BYTES + 8]
        f.seek(-4, 2)
        assert f.read(8) == H.FILE_DATA[-4:]
        f.seek(0)
        whole = f.read()
    assert whole == H.FILE_DATA, "full-file read mismatch"
    print("   open/read/seek/tell/close over a fragmented chain: %d bytes "
          "byte-exact" % len(whole))

    for bad, code in (("/sd/nope.kmodel", 2),):
        try:
            vfs.open(bad, "rb")
            raise AssertionError("missing file must raise")
        except OSError as e:
            assert e.args[0] == code, e
    try:
        vfs.open("/sd/" + H.LONG_NAME, "wb")
        raise AssertionError("write mode must raise")
    except OSError as e:
        assert e.args[0] == 30, e
    print("   ENOENT for missing file, EROFS for write modes")

    print("== 4. main() picks the VFS path ==")
    ns2 = load()
    out = []
    ns2["p"] = lambda m: out.append(m)
    ns2["MOUNT_TRY"] = 1
    ns2["main"]()
    joined = "\n".join(out)
    assert "register_vfs OK" in joined, joined
    assert "RESULT: PASS" in joined, joined
    assert any(c[0] == "mount" for c in ns2["_calls"]), ns2["_calls"]
    assert any(c[0] == "register_vfs" for c in ns2["_calls"]), ns2["_calls"]
    print("   uos.mount failed -> register_vfs -> PASS (%d log lines)"
          % len(out))

    print("\nALL VFS/BLOCKDEV CHECKS PASSED")


main()
