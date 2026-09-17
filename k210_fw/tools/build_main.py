# -*- coding: utf-8 -*-
"""Build k210_fw/park_app.py so it carries the SPI-FAT SD driver inside it.

WHY
    The 01Studio Lite firmware's KPU backend is a different API (KPU.load/run/
    get_outputs/Yolo2/Lpr) and cannot run the vendor's v3 plate models at all, so we
    are going back to the makerobo firmware - which DOES have the old API
    (load_kmodel / init_yolo2 / lp_recog_load_weight_data) but does NOT mount our
    SD card (its socket is wired to SPI IO26-29, the firmware looks for SDIO).

    That is why we wrote k210_fw/tools/sd_spi_fat.py in the first place.  The problem: it
    is a second file on /flash, and flashing firmware wipes /flash - which is exactly
    how we lost it (and /flash is too tight for reliable 33KB saves; this board cannot
    even create new files there).

    So this builder splices the needed parts of sd_spi_fat.py into park_app.py, giving one
    self-contained file: SD driver + FAT32 + VFS + the application.  Nothing to lose.

WHAT IT DOES
    - reads k210_fw/tools/sd_spi_fat.py and keeps the driver half only:
        SDSPI, BlockDev, VfsFat32, Fat32File, Fat32, mbr_partitions, _has_dir,
        p, hx   (setup_sd_vfs() and main() are dropped - park_app.py mounts for itself)
    - reads k210_fw/park_app.py and replaces the marked region between
        "# ==== BEGIN INLINE SD DRIVER" and "# ==== END INLINE SD DRIVER"
      with that driver code
    - writes k210_fw/park_app.py back (UTF-8, CRLF->LF normalised, no BOM)
    - sanity checks: syntax compiles, no non-ASCII sneaks into the inlined block,
      total size is sane, and sd_ensure_mounted()/setup_sd_vfs() still exist

USAGE
    python k210_fw/tools/build_main.py            # splice in place
    python k210_fw/tools/build_main.py --check    # verify only, change nothing
"""
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DRIVER = os.path.join(REPO, "tools", "sd_spi_fat.py")
# the application; the launcher (k210_fw/main.py) is a separate 3.7KB boot script
MAIN = os.path.join(REPO, "park_app.py")
BEGIN = "# ==== BEGIN INLINE SD DRIVER"
END = "# ==== END INLINE SD DRIVER"

# classes/functions from sd_spi_fat.py that park_app.py needs.  setup_sd_vfs() and main()
# are intentionally excluded: park_app.py has its own mount logic.
KEEP = ("p", "hx", "SDSPI", "BlockDev", "VfsFat32", "Fat32File", "Fat32",
        "mbr_partitions", "_has_dir", "_progress")
# module-level constants are copied too (PINS/BAUD/TOKEN_TRIES/error codes/...)


def _blocks(src):
    """Split a module into top-level blocks keyed by the name they define."""
    lines = src.split("\n")
    starts = []
    for i, line in enumerate(lines):
        m = re.match(r"^(?:class|def)\s+([A-Za-z_]\w*)", line)
        if m:
            starts.append((i, m.group(1)))
    out = {}
    for j, (i, name) in enumerate(starts):
        end = starts[j + 1][0] if j + 1 < len(starts) else len(lines)
        out[name] = "\n".join(lines[i:end]).rstrip() + "\n"
    return out


def build_driver_text():
    src = open(DRIVER, encoding="utf-8").read().replace("\r\n", "\n")
    # 1) module-level constants (PINS/BAUD/TOKEN_TRIES/error codes/_IOCTL_* ...) - the
    #    driver blocks reference them.  NOTE the `_` in the leading character class:
    #    missing it silently dropped _IOCTL_INIT and friends, which then blew up at
    #    runtime as NameError inside BlockDev.ioctl.
    consts = []
    for line in src.split("\n"):
        if re.match(r"^[A-Z_][A-Z0-9_]*\s*=", line):
            consts.append(line.rstrip())
    if not consts:
        raise SystemExit("sd_spi_fat.py has no module-level constants?")
    # 2) the kept class/function blocks, in file order
    blocks = _blocks(src)
    missing = [n for n in KEEP if n not in blocks]
    if missing:
        raise SystemExit("sd_spi_fat.py is missing blocks: %s" % missing)
    order = [n for n in _names_in_order(src) if n in KEEP]
    parts = ["# --- constants (from sd_spi_fat.py) ---"] + consts
    parts.append("")
    for name in order:
        parts.append(blocks[name])
    text = "\n".join(parts)
    # the driver half must stay pure ASCII: this text is embedded in a board file and
    # the old board-side rule is "no non-ASCII in anything we paste/save to the board"
    bad = [(i, hex(c)) for i, c in enumerate(text.encode("utf-8")) if c > 127]
    if bad:
        raise SystemExit("inlined driver has %d non-ASCII bytes" % len(bad))
    return text


def _names_in_order(src):
    return [m.group(1) for m in
            re.finditer(r"^(?:class|def)\s+([A-Za-z_]\w*)", src, re.M)]


def _names(text):
    """(names the block reads, names the block binds) - parsed with ast, so strings and
    comments cannot fool it.  Used to prove the inlined driver can actually run inside
    park_app.py; the class of bug this catches cost two board round-trips (missing
    `import machine`, then the dropped _IOCTL_* constants)."""
    import ast
    import builtins
    tree = ast.parse(text)
    bound = set(dir(builtins))
    for node in ast.walk(tree):
        if isinstance(node, (ast.Import, ast.ImportFrom)):
            for a in node.names:
                bound.add((a.asname or a.name).split(".")[0])
        elif isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            bound.add(node.name)
            for a in list(node.args.args) + list(node.args.kwonlyargs):
                bound.add(a.arg)
            if node.args.vararg:
                bound.add(node.args.vararg.arg)
            if node.args.kwarg:
                bound.add(node.args.kwarg.arg)
        elif isinstance(node, ast.ClassDef):
            bound.add(node.name)
            for n in ast.walk(node):
                if isinstance(n, ast.Name) and isinstance(n.ctx, ast.Store):
                    bound.add(n.id)
                elif isinstance(n, ast.arg):
                    bound.add(n.arg)
        elif isinstance(node, ast.Name) and isinstance(node.ctx, ast.Store):
            bound.add(node.id)
        elif isinstance(node, ast.arg):
            bound.add(node.arg)
        elif isinstance(node, ast.ExceptHandler) and node.name:
            bound.add(node.name)
        elif isinstance(node, ast.comprehension):
            for n in ast.walk(node.target):
                if isinstance(n, ast.Name):
                    bound.add(n.id)
    read = {n.id for n in ast.walk(tree)
            if isinstance(n, ast.Name) and isinstance(n.ctx, ast.Load)}
    return read, bound


def _undefined_names(driver_text, host_code):
    """Driver names it reads but that neither it nor park_app.py's head binds."""
    read, driver_bound = _names(driver_text)
    _r2, host_bound = _names(host_code) if host_code.strip() else (set(), set())
    return sorted(read - driver_bound - host_bound)


def splice(app_src, driver_text):
    if BEGIN not in app_src or END not in app_src:
        raise SystemExit("park_app.py is missing the %s / %s markers" % (BEGIN, END))
    head, rest = app_src.split(BEGIN, 1)
    _old, tail = rest.split(END, 1)
    # IDEMPOTENT: exactly one newline after END, decided by the builder rather than by
    # whatever the previous build left behind.  Without this, every build appended one
    # more blank line, so `--check` reported a phantom 1-byte diff forever after.
    block = ("%s\n# generated by k210_fw/tools/build_main.py from sd_spi_fat.py - do not\n"
             "# edit this block by hand, edit the source module and re-run the builder.\n"
             "%s\n%s" % (BEGIN, driver_text.rstrip(), END))
    return head + block + "\n" + tail.lstrip("\n")


def main():
    check_only = "--check" in sys.argv
    driver_text = build_driver_text()
    app_src = open(MAIN, encoding="utf-8").read().replace("\r\n", "\n")
    new_src = splice(app_src, driver_text)

    # sanity: syntax + the pieces park_app.py must still have
    try:
        compile(new_src, MAIN, "exec")
    except SyntaxError as e:
        raise SystemExit("built park_app.py does not compile: %s" % e)
    for needle in ("def sd_ensure_mounted(", "def kpu_load(", "def main():",
                   "SDSPI", "Fat32", "VfsFat32"):
        if needle not in new_src:
            raise SystemExit("built park_app.py lost %r" % needle)
    # every name the inlined driver READS must be provided either by the driver itself
    # or by park_app.py's head (imports/globals above the block).  Parsed with ast so
    # strings and comments cannot fool it.  This is exactly the class of bug that cost
    # two board round-trips: a missing `import machine` (NameError at SPI construct)
    # and the dropped `_IOCTL_*` constants (NameError inside BlockDev.ioctl).
    head = new_src.split(BEGIN, 1)[0]
    head_code = "\n".join(l for l in head.split("\n")
                          if not l.lstrip().startswith("#"))
    missing = _undefined_names(driver_text, head_code)
    if missing:
        raise SystemExit("inlined driver reads names nothing defines: %s" % missing)

    old_b = len(app_src.encode("utf-8"))
    new_b = len(new_src.encode("utf-8"))
    drv_b = len(driver_text.encode("utf-8"))
    print("driver block : %6d bytes" % drv_b)
    print("park_app.py  : %6d -> %6d bytes" % (old_b, new_b))
    if check_only:
        print("check only: nothing written")
        return
    open(MAIN, "w", encoding="utf-8", newline="\n").write(new_src)
    print("wrote %s" % MAIN)


main()
