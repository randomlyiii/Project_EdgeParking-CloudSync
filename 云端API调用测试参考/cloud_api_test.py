#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Cloud API call test for the EdgeParking cloud fallback path (step 7 / P7-01).

What it does
    Sends the very same request that Core1 (Qt park_ui, CloudClient) sends in
    production to an OpenAI compatible /chat/completions endpoint:

        POST <api_base>
        Content-Type: application/json
        Authorization: Bearer <api_key>
        {"model":"<model>","messages":[{"role":"user","content":[
            {"type":"text","text":"<prompt>"},
            {"type":"image_url","image_url":{"url":"data:image/jpeg;base64,..."}}]}],
         "max_tokens":64,"temperature":0}

    ...then parses the answer with the same three level tolerant parser as
    cloud_client.cpp (strip ``` fences -> balanced brace scan -> JSON) so a
    green run here proves the board side contract as well.

Configuration files next to this script (privacy split, 2026-09-11):
    sample_key.txt  -> PUBLIC, uploaded to GitHub: api_key="sk-xxx" is only a
                       placeholder (refused as a key); it documents the shape
                       and supplies base_url as a reference
    key.txt         -> PRIVATE, listed in the repo .gitignore: the real
                       api_key, optionally api_base / base_url
    车牌.jpg        -> the sample plate image (also: --image <path>)

    The real key is therefore read from key.txt (fallback: $DASHSCOPE_API_KEY).
    It never lands in this script, in sample_key.txt, in git, or in the logs:
    every log line and --json-out only carry the mask (sk-7b8b...c901f).

Why the standard library only
    This PC has C:\\Users\\<user>\\AppData\\Local\\Programs\\Python\\Python314 and
    NO pip and NO third party packages (openai/requests/PIL all missing), so the
    harness uses urllib + ssl + base64 only: it runs by double clicking
    run_test.bat, with zero installation. --selftest serves its canned answers
    through a local http.server on 127.0.0.1 (plain http, no TLS material to
    ship), because OpenSSL needs a real certificate to spin up an https server.

Usage (short form: run_test.bat, full form: python cloud_api_test.py --help)
    --mode recognize   one real image round trip, prints the parsed plate
    --mode test        cheapest text-only round trip (endpoint + key alive?)
    --mode info        image header/dimension report, no network
    --mode selftest    ~75 offline checks of the parser, the HTTP path and the
                       key-source privacy rules
    --mode models      picture-capable model variants, ends with a "ps" marker
    --json-out FILE    machine readable result
    --raw FILE         raw API response body (for bug reports)
    --show-body        print the outgoing request with the image truncated

Exit codes: 0 accepted / 1 failed / 2 unreadable / 3 usage or environment error.
"""

import argparse
import base64
import datetime
import http.server
import json
import os
import re
import shutil
import ssl
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))

DEFAULT_BASE = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"
DEFAULT_MODEL = "qwen3-vl-plus"
DEFAULT_IMAGE = "车牌.jpg"
KEY_FILE = "key.txt"
SAMPLE_FILE = "sample_key.txt"
KEY_ENV = "DASHSCOPE_API_KEY"

# Identical to CloudSettings defaults in core1_ui/qt_gui/src/cloud_settings.cpp
DEFAULT_PROMPT = (
    "You are a license plate OCR engine for a parking gate. Look at the "
    "image and reply with ONLY one JSON object, no prose, no markdown fences: "
    '{"plate":"<province short name + letter + 5 chars>","confidence":<0..1>}. '
    "If no plate is readable reply "
    '{"plate":"","confidence":0}.'
)

MAX_PLATE_BYTES = 15          # park_shm plate[16]
ACCEPT_CONF = 0.50            # cloud.conf accept_conf
TRIGGER_CONF = 0.60           # cloud.conf trigger_conf (only reported here)
PLATE_CHARS = "京津冀晋蒙辽吉黑沪苏浙皖闽赣鲁豫鄂湘粤桂琼渝川贵云藏陕甘青宁新" \
              "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
# Cosmetic separators the models like to insert between the province letter and
# the body ("川A·88888", "川A-88888"). core0's whitelist matches plates byte for
# byte (business/whitelist.c wl_match -> strcmp), so "川A·88888" would never hit
# a "川A88888" entry and the gate would stay closed: drop them here.
PLATE_SEPARATORS = "·•・.。-_ \t\r\n"

MODELS = [
    ("qwen3-vl-plus", "vision, current generation (recommended)"),
    ("qwen3-vl-flash", "vision, current generation, cheapest"),
    ("qwen-vl-max", "vision, previous generation, best accuracy"),
    ("qwen-vl-plus", "vision, previous generation, cheaper"),
    ("qwen-vl-ocr", "vision, OCR specialized"),
]
TEXT_MODELS = [
    ("qwen-plus", "text only, cheapest connectivity proof"),
    ("qwen-turbo", "text only, fastest"),
]

# Families that exist on DashScope but CANNOT serve chat/completions image
# requests. The API answers a bare 404 "Unsupported model" for these, which is
# easy to misread as a wrong endpoint.
NON_CHAT_FAMILIES = [
    ("wan", "text/image-to-video generation (wan2.x-t2v / wanx / i2v)"),
    ("wanx", "image generation (wanx-v1, wanx2.x-t2i)"),
    ("cosyvoice", "speech synthesis"),
    ("sambert", "speech synthesis"),
    ("paraformer", "speech recognition"),
    ("sensevoice", "speech recognition"),
    ("qwen-audio", "audio understanding"),
    ("stable-diffusion", "image generation"),
    ("flux", "image generation"),
    ("emo", "talking head video"),
    ("animate", "image to video"),
]

EXIT_ACCEPTED, EXIT_FAILED, EXIT_UNREADABLE, EXIT_USAGE = 0, 1, 2, 3

VERBOSE = False

# The plate itself is Chinese (京A12345), so the console must not encode it as
# "?" - force UTF-8 on both streams. run_test.bat already did "chcp 65001";
# doing it here as well keeps a bare "python cloud_api_test.py" correct.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):
        pass


# --------------------------------------------------------------------------
# small helpers


def log(msg):
    ts = datetime.datetime.now().strftime("%H:%M:%S")
    line = "[%s] %s\n" % (ts, msg)
    try:
        sys.stdout.write(line)
    except UnicodeEncodeError:
        # legacy console code page: keep it readable instead of crashing
        sys.stdout.write(ascii_fold(line))
    sys.stdout.flush()


def vlog(msg):
    if VERBOSE:
        log(msg)


def mask_key(key):
    if not key:
        return "(no key)"
    if len(key) <= 10:
        return key[:2] + "..." + key[-2:]
    return "%s...%s (%d chars)" % (key[:7], key[-5:], len(key))


def ascii_fold(s):
    """Fallback rendering for consoles that cannot encode the text at all."""
    return s.encode("ascii", "replace").decode("ascii")


def cjk_printable():
    """True when stdout can carry Chinese (plate) text without mangling it.

    A piped stdout on a Chinese Windows reports gbk, and anything captured that
    way turns into mojibake (observed on this PC), so the plate is then also
    printed as \\uXXXX escapes while the true value still goes to --json-out.
    """
    enc = getattr(sys.stdout, "encoding", None) or "ascii"
    try:
        "粤".encode(enc)
        return True
    except (UnicodeEncodeError, LookupError):
        return False


def selftest_tmpdir():
    """Scratch dir for the offline checks, inside the workspace on purpose.

    This agent sandbox allows writes under the workspace only: tempfile's
    default (%TEMP%) raises PermissionError, so keep the throwaway config files
    next to the script instead.
    """
    path = os.path.join(HERE, ".selftest_tmp")
    try:
        os.makedirs(path, exist_ok=True)
    except OSError:
        path = tempfile.mkdtemp(prefix="park_selftest_")
    return path


def plate_display(plate):
    """ASCII safe plate rendering: 粤B12345 -> \\u7ca4B12345 when needed."""
    if not plate or cjk_printable():
        return plate if plate else "(empty)"
    return "".join(c if ord(c) < 128 else "\\u%04x" % ord(c) for c in plate)


def text_safe(text):
    """Escape non-ASCII for consoles that cannot render it (model answers)."""
    if cjk_printable():
        return text
    return "".join(c if ord(c) < 128 else "\\u%04x" % ord(c) for c in text)


def short(s, n=200):
    s = s.replace("\r", " ").replace("\n", " ")
    return s if len(s) <= n else s[:n] + "..."


def truncate_utf8(s, max_bytes):
    """Same contract as CloudClient::truncateUtf8 (park_shm plate[16])."""
    out = []
    used = 0
    for ch in s:
        n = len(ch.encode("utf-8"))
        if used + n > max_bytes:
            break
        out.append(ch)
        used += n
    return "".join(out)


def now_iso():
    return datetime.datetime.now().astimezone().isoformat(timespec="seconds")


# --------------------------------------------------------------------------
# configuration loading


def parse_sample_base(path):
    """sample_key.txt holds a python snippet; take base_url from it."""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError:
        return None
    m = re.search(r"""base_url\s*=\s*["']([^"']+)["']""", text)
    if not m:
        return None
    base = m.group(1).strip().rstrip("/")
    if base.endswith("/chat/completions"):
        return base
    return base + "/chat/completions"


def parse_sample_key(path):
    """The placeholder in sample_key.txt must never be used as a real key."""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError:
        return None
    m = re.search(r"""api_key\s*=\s*["']([^"']+)["']""", text)
    if not m:
        return None
    key = m.group(1).strip()
    if not key or "xxx" in key.lower() or key in ("sk-", "sk-xxx"):
        return None
    return key


def parse_model_field(path):
    """Optional model="..." hint in a config snippet (sample or key file).

    This is only a default: --model and key.txt still win, because the public
    sample is a reference file, not a settings source.
    """
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError:
        return None
    m = re.search(r"""["']?model["']?\s*[:=]\s*["']([^"']+)["']""", text)
    if not m:
        return None
    model = m.group(1).strip()
    return model or None


def parse_bare_key(path):
    """A key file may just hold one bare "sk-..." token."""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError:
        return None
    for line in text.splitlines():
        line = line.strip().strip('"').strip("'")
        if line.startswith("sk-") and " " not in line and len(line) >= 20:
            return line
    return None


def load_config(args):
    """Resolve endpoint + key.

    Privacy model (2026-09-11): this folder is uploaded to GitHub, so
        sample_key.txt = the PUBLIC sanitised snippet (api_key is "sk-xxx")
        key.txt        = the REAL key + api_base, listed in the repo .gitignore
    The key is therefore read from key.txt, never from sample_key.txt: the
    sample only supplies the endpoint and documents the expected shape.
    Last resort for a key: $DASHSCOPE_API_KEY (and even then the sample's
    "sk-xxx" placeholder is refused by parse_sample_key).

    Precedence: --key > $DASHSCOPE_API_KEY > key.txt > sample_key.txt.
    """
    cfg = {
        "base": DEFAULT_BASE,
        "key": "",
        "key_src": "",
        "model": args.model or DEFAULT_MODEL,
        "image": os.path.join(HERE, args.image or DEFAULT_IMAGE),
        "prompt": DEFAULT_PROMPT,
    }
    sample = os.path.join(HERE, SAMPLE_FILE)
    key_file = args.key_file or os.path.join(HERE, KEY_FILE)

    # ---- model ----------------------------------------------------------
    # Precedence: --model > key.txt model= > sample_key.txt model= > default
    cfg["model_src"] = "built-in default"
    if os.path.isfile(key_file):
        from_key_file = parse_model_field(key_file)
        if from_key_file:
            cfg["model"] = from_key_file
            cfg["model_src"] = "%s (model)" % os.path.basename(key_file)
    if cfg["model_src"] == "built-in default":
        from_sample = parse_model_field(sample)
        if from_sample:
            cfg["model"] = from_sample
            cfg["model_src"] = "%s (model)" % SAMPLE_FILE
    if args.model:
        cfg["model"] = args.model
        cfg["model_src"] = "--model"

    # ---- endpoint -------------------------------------------------------
    cfg["base_src"] = "built-in default"
    if os.path.isfile(key_file):
        from_key_file = parse_sample_base(key_file)
        if from_key_file:
            cfg["base"] = from_key_file
            cfg["base_src"] = "%s (api_base)" % os.path.basename(key_file)
    if cfg["base_src"] == "built-in default":
        from_sample = parse_sample_base(sample)
        if from_sample:
            cfg["base"] = from_sample
            cfg["base_src"] = "%s (reference)" % SAMPLE_FILE
    if args.base:
        cfg["base"] = args.base.rstrip("/")
        cfg["base_src"] = "--base"
    if not cfg["base"].endswith("/chat/completions"):
        cfg["base"] += "/chat/completions"

    # ---- api key --------------------------------------------------------
    if args.key:
        cfg["key"] = args.key.strip()
        cfg["key_src"] = "--key (inline)"
    else:
        env = os.environ.get(KEY_ENV, "").strip()
        if os.path.isfile(key_file):
            # key.txt accepts three shapes: a bare "sk-..." line, an
            # api_key="sk-..." snippet, or the whole OpenAI(...) block.
            cfg["key"] = parse_sample_key(key_file) or parse_bare_key(key_file)
            cfg["key_src"] = "%s (private, git-ignored)" % os.path.basename(
                key_file)
            if not cfg["key"]:
                cfg["key_src"] = ("%s (no usable key found)"
                                  % os.path.basename(key_file))
        elif env:
            # No private file at all: the environment is the safer source.
            cfg["key"] = env
            cfg["key_src"] = "$%s" % KEY_ENV
        else:
            # Public sample only; parse_sample_key refuses "sk-xxx".
            sample_key = parse_sample_key(sample)
            if sample_key:
                cfg["key"] = sample_key
                cfg["key_src"] = "%s (public sample!)" % SAMPLE_FILE
    if args.prompt:
        cfg["prompt"] = args.prompt
    return cfg


# --------------------------------------------------------------------------
# image handling (no PIL on this PC: decode the JPEG/PNG header by hand)


def sniff_image(path):
    with open(path, "rb") as fh:
        head = fh.read(32)
        fh.seek(0, os.SEEK_END)
        size = fh.tell()
    if head[:3] == b"\xff\xd8\xff":
        return "jpeg", size
    if head[:8] == b"\x89PNG\r\n\x1a\n":
        return "png", size
    if head[:6] in (b"GIF87a", b"GIF89a"):
        return "gif", size
    if head[:4] == b"RIFF" and head[8:12] == b"WEBP":
        return "webp", size
    return None, size


def image_dimensions(path, kind):
    """Minimal header scan: enough to prove the file is a real photo."""
    with open(path, "rb") as fh:
        data = fh.read(65536)
    if kind == "jpeg":
        i = 2
        while i + 9 < len(data):
            if data[i] != 0xFF:
                i += 1
                continue
            marker = data[i + 1]
            if marker in (0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7,
                          0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF):
                h = (data[i + 5] << 8) | data[i + 6]
                w = (data[i + 7] << 8) | data[i + 8]
                return w, h
            if marker in (0xD8, 0xD9, 0x01) or 0xD0 <= marker <= 0xD7:
                i += 2
                continue
            seg = (data[i + 2] << 8) | data[i + 3]
            i += 2 + seg
    if kind == "png":
        w = int.from_bytes(data[16:20], "big")
        h = int.from_bytes(data[20:24], "big")
        return w, h
    return 0, 0


# --------------------------------------------------------------------------
# request / response (mirrors cloud_client.cpp)


def build_request_body(model, prompt, image_b64, jpeg=True):
    content = [{"type": "text", "text": prompt}]
    if image_b64:
        mime = "image/jpeg" if jpeg else "image/png"
        content.append({
            "type": "image_url",
            "image_url": {"url": "data:%s;base64,%s" % (mime, image_b64)},
        })
    msg = {"role": "user", "content": content}
    return {
        "model": model,
        "messages": [msg],
        "max_tokens": 64,
        "temperature": 0,
    }


class Response(object):
    def __init__(self):
        self.status = 0
        self.body = ""
        self.error = ""
        self.ms = 0
        self.headers = {}
        self.host = ""
        self.sent_bytes = 0


def http_post(url, api_key, payload, timeout):
    """POST json with the production headers; never raises."""
    out = Response()
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    out.sent_bytes = len(body)
    parsed = urllib.parse.urlsplit(url)
    out.host = parsed.netloc
    req = urllib.request.Request(url, data=body, method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("Accept", "application/json")
    req.add_header("Authorization", "Bearer " + api_key)
    req.add_header("User-Agent", "park-cloud-test/1.0")
    ctx = ssl.create_default_context()
    if parsed.scheme == "https" and len(ctx.get_ca_certs()) == 0:
        vlog("WARNING: no CA certificates loaded, TLS verification will fail")
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=timeout, context=ctx) as resp:
            out.status = resp.status
            out.body = resp.read().decode("utf-8", "replace")
            out.headers = dict(resp.headers.items())
    except urllib.error.HTTPError as exc:
        # An HTTP error still carries a JSON error envelope we want to classify.
        out.status = exc.code
        try:
            out.body = exc.read().decode("utf-8", "replace")
        except Exception:
            out.body = ""
        out.headers = dict(exc.headers.items()) if exc.headers else {}
        out.error = "http %d" % exc.code
    except urllib.error.URLError as exc:
        out.status = -1
        out.error = "network: %s" % (exc.reason,)
    except (TimeoutError, OSError, ValueError) as exc:
        out.status = -1
        out.error = "network: %s" % (exc,)
    out.ms = int((time.time() - t0) * 1000)
    return out


def strip_fences(text):
    """Level 1 of the tolerant parser (identical rules to extractContent)."""
    text = text.strip()
    if text.startswith("```"):
        nl = text.find("\n")
        if nl > 0:
            text = text[nl + 1:]
        end = text.rfind("```")
        if end >= 0:
            text = text[:end]
    return text.strip()


def extract_content(body, err_out):
    """Level 0/1: envelope -> choices[0].message.content, or api error text."""
    try:
        root = json.loads(body)
    except (ValueError, TypeError):
        err_out.append("bad response envelope")
        return None
    if not isinstance(root, dict):
        err_out.append("bad response envelope")
        return None
    if "error" in root and root["error"]:
        ev = root["error"]
        if isinstance(ev, dict):
            msg = ev.get("message") or ev.get("code") or "api error"
        else:
            msg = str(ev)
        err_out.append("api error: %s" % short(str(msg), 120))
        err_out.append(str(ev.get("code") if isinstance(ev, dict) else ""))
        return None
    choices = root.get("choices") or []
    if not isinstance(choices, list) or not choices:
        err_out.append("no choices in response")
        return None
    first = choices[0] if isinstance(choices[0], dict) else {}
    message = first.get("message") or {}
    text = message.get("content")
    if isinstance(text, list):
        # some providers answer with content parts
        parts = [p.get("text", "") for p in text if isinstance(p, dict)]
        text = "".join(parts)
    if not text:
        err_out.append("empty content")
        return None
    return strip_fences(str(text))


def find_json_object(text):
    """Level 2: first balanced {...}, ignoring braces inside strings."""
    start = text.find("{")
    if start < 0:
        return None
    depth = 0
    in_str = False
    esc = False
    for i in range(start, len(text)):
        c = text[i]
        if in_str:
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == '"':
                in_str = False
            continue
        if c == '"':
            in_str = True
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    return None


def parse_plate(content, err_out):
    """Level 2/3: -> (plate, confidence). Empty plate means 'unreadable'."""
    raw = find_json_object(content)
    if raw is None:
        err_out.append("no json object in answer")
        return None
    try:
        obj = json.loads(raw)
    except ValueError as exc:
        err_out.append("json parse error: %s" % exc)
        return None
    if not isinstance(obj, dict):
        err_out.append("json is not an object")
        return None
    plate = obj.get("plate")
    if plate is None:
        # tolerate a couple of common aliases
        for alias in ("plate_no", "plateNo", "number", "text", "车牌"):
            if alias in obj:
                plate = obj[alias]
                break
    if not isinstance(plate, str):
        plate = "" if plate is None else str(plate)
    for ch in PLATE_SEPARATORS:
        plate = plate.replace(ch, "")
    plate = truncate_utf8(plate, MAX_PLATE_BYTES)

    conf = obj.get("confidence", obj.get("score", 0))
    try:
        conf = float(conf)
    except (TypeError, ValueError):
        conf = 0.0
    if conf > 1.0:
        conf = 1.0
    if conf < 0.0:
        conf = 0.0
    return plate, conf


def plate_looks_valid(plate):
    """Whitelist check against the demo's plate charset (advisory only)."""
    if not plate:
        return False, "empty"
    for ch in plate:
        if ch not in PLATE_CHARS:
            return False, "char %r outside the demo charset" % ch
    if len(plate) < 6:
        return False, "too short (%d)" % len(plate)
    return True, "ok"


# --------------------------------------------------------------------------
# result plumbing


class Result(object):
    def __init__(self):
        self.mode = ""
        self.started = now_iso()
        self.elapsed_ms = 0
        self.base = ""
        self.base_src = ""
        self.model = ""
        self.model_src = ""
        self.key_src = ""
        self.key_mask = ""
        self.image = ""
        self.image_kind = ""
        self.image_bytes = 0
        self.image_w = 0
        self.image_h = 0
        self.sent_bytes = 0
        self.http_status = 0
        self.api_code = ""
        self.content = ""
        self.plate = ""
        self.confidence = 0.0
        self.classification = ""
        self.reason = ""
        self.detail = ""
        self.checks = []

    def as_dict(self):
        return {
            "mode": self.mode,
            "started": self.started,
            "elapsed_ms": self.elapsed_ms,
            "base": self.base,
            "base_src": self.base_src,
            "model": self.model,
            "model_source": self.model_src,
            "key_source": self.key_src,
            "key_mask": self.key_mask,
            "image": self.image,
            "image_kind": self.image_kind,
            "image_bytes": self.image_bytes,
            "image_size": [self.image_w, self.image_h],
            "request_bytes": self.sent_bytes,
            "http_status": self.http_status,
            "api_code": self.api_code,
            "content": self.content,
            "plate": self.plate,
            "confidence": self.confidence,
            "classification": self.classification,
            "reason": self.reason,
            "detail": self.detail,
            "checks": self.checks,
        }


def classify(res):
    """accepted / unreadable / failed - same three classes as Core1."""
    if res.http_status < 0 or res.http_status >= 400 or res.reason:
        res.classification = "failed"
        return EXIT_FAILED
    if not res.plate:
        res.classification = "unreadable"
        return EXIT_UNREADABLE
    if res.confidence < ACCEPT_CONF:
        res.classification = "unreadable"
        res.reason = ("confidence %.2f below accept_conf %.2f"
                      % (res.confidence, ACCEPT_CONF))
        return EXIT_UNREADABLE
    res.classification = "accepted"
    return EXIT_ACCEPTED


# --------------------------------------------------------------------------
# modes


def mode_info(args, cfg, res):
    res.mode = "info"
    if not os.path.isfile(cfg["image"]):
        log("ERROR: image not found: %s" % ascii_fold(cfg["image"]))
        return EXIT_USAGE
    kind, size = sniff_image(cfg["image"])
    res.image = cfg["image"]
    res.image_kind = kind or "unknown"
    res.image_bytes = size
    if kind:
        res.image_w, res.image_h = image_dimensions(cfg["image"], kind)
    log("image    : %s" % ascii_fold(cfg["image"]))
    log("kind     : %s" % res.image_kind)
    log("size     : %d bytes" % size)
    log("pixels   : %dx%d" % (res.image_w, res.image_h))
    if kind is None:
        log("ERROR: not a JPEG/PNG/GIF/WebP file - the model would reject it")
        return EXIT_USAGE
    log("base64   : ~%d chars (q70 JPEG on the board is ~10KB -> ~13KB body)"
        % ((size + 2) // 3 * 4))
    res.image = cfg["image"]
    res.classification = "info"
    return EXIT_ACCEPTED


def mode_test(args, cfg, res):
    """Cheapest possible round trip: text only, proves endpoint + key."""
    res.mode = "test"
    if not cfg["key"]:
        log("ERROR: no api key (fill %s or set $%s)" % (KEY_FILE, KEY_ENV))
        return EXIT_USAGE
    res.base = cfg["base"]
    res.base_src = cfg["base_src"]
    res.model = cfg["model"] if args.model else "qwen-plus"
    res.key_src = cfg["key_src"]
    res.key_mask = mask_key(cfg["key"])
    log("endpoint : %s (%s)" % (res.base, res.base_src))
    log("model    : %s" % res.model)
    log("key      : %s from %s" % (res.key_mask, res.key_src))
    payload = {
        "model": res.model,
        "messages": [{"role": "user", "content": "Reply with: ok"}],
        "max_tokens": 4,
        "temperature": 0,
    }
    if args.show_body:
        print(json.dumps(payload, ensure_ascii=False, indent=2))
    log("POST ... (timeout %ds)" % args.timeout)
    resp = http_post(res.base, cfg["key"], payload, args.timeout)
    res.http_status = resp.status
    res.elapsed_ms = resp.ms
    res.sent_bytes = resp.sent_bytes
    if args.raw:
        write_raw(args.raw, resp.body)
    if resp.status < 0:
        res.reason = "network error"
        res.detail = resp.error
        log("FAILED   : %s" % resp.error)
        return classify(res)
    errs = []
    content = extract_content(resp.body, errs) if resp.status < 400 else None
    if resp.status >= 400 or content is None:
        errs = []
        extract_content(resp.body, errs)
        res.api_code = errs[1] if len(errs) > 1 else ""
        res.reason = errs[0] if errs else "http %d" % resp.status
        res.detail = short(resp.body, 300)
        log("http     : %d in %d ms (sent %d B)"
            % (resp.status, resp.ms, resp.sent_bytes))
        log("FAILED   : %s" % res.reason)
        log("body     : %s" % short(resp.body, 300))
        explain_http(resp.status, resp.body)
        return classify(res)
    res.content = content
    log("http     : %d in %d ms (sent %d B)" % (resp.status, resp.ms,
                                                resp.sent_bytes))
    log("answer   : %s" % short(content, 120))
    log("RESULT   : endpoint + key are OK (model=%s)" % res.model)
    res.classification = "accepted"
    res.reason = ""
    return EXIT_ACCEPTED


def mode_recognize(args, cfg, res):
    res.mode = "recognize"
    if not cfg["key"]:
        log("ERROR: no api key (fill %s or set $%s)" % (KEY_FILE, KEY_ENV))
        return EXIT_USAGE
    if not os.path.isfile(cfg["image"]):
        log("ERROR: image not found: %s" % ascii_fold(cfg["image"]))
        return EXIT_USAGE
    kind, size = sniff_image(cfg["image"])
    if kind is None:
        log("ERROR: %s is not a JPEG/PNG image" % ascii_fold(cfg["image"]))
        return EXIT_USAGE
    res.image = cfg["image"]
    res.image_kind = kind
    res.image_bytes = size
    res.image_w, res.image_h = image_dimensions(cfg["image"], kind)
    res.base = cfg["base"]
    res.base_src = cfg["base_src"]
    res.model = cfg["model"]
    res.key_src = cfg["key_src"]
    res.key_mask = mask_key(cfg["key"])

    log("endpoint : %s (%s)" % (res.base, res.base_src))
    log("model    : %s" % res.model)
    log("key      : %s from %s" % (res.key_mask, res.key_src))
    log("image    : %s %s %dx%d %d bytes"
        % (ascii_fold(os.path.basename(cfg["image"])), kind, res.image_w,
           res.image_h, size))

    t0 = time.time()
    with open(cfg["image"], "rb") as fh:
        image_b64 = base64.b64encode(fh.read()).decode("ascii")
    payload = build_request_body(res.model, cfg["prompt"], image_b64,
                                 jpeg=(kind == "jpeg"))
    res.sent_bytes = len(json.dumps(payload, ensure_ascii=False).encode("utf-8"))
    log("body     : %d bytes (image base64 %d chars, max_tokens=64, temp=0)"
        % (res.sent_bytes, len(image_b64)))
    if args.show_body:
        print(json.dumps(strip_image_b64(payload), ensure_ascii=False, indent=2))
    log("POST ... (timeout %ds)" % args.timeout)
    resp = http_post(res.base, cfg["key"], payload, args.timeout)
    # keep the reported time limited to the HTTP round trip, like Core1 does
    res.elapsed_ms = resp.ms
    res.http_status = resp.status
    if args.raw:
        write_raw(args.raw, resp.body)

    if resp.status < 0:
        res.reason = "network error"
        res.detail = resp.error
        log("FAILED   : %s" % resp.error)
        log("hint     : DNS/proxy/TLS. Board side has no CA bundle, see "
            "docs/protocols.md 5.4")
        return classify(res)

    log("http     : %d in %d ms" % (resp.status, resp.ms))
    if resp.status >= 400:
        errs = []
        extract_content(resp.body, errs)
        res.api_code = errs[1] if len(errs) > 1 else ""
        res.reason = errs[0] if errs else "http %d" % resp.status
        res.detail = short(resp.body, 300)
        log("FAILED   : %s" % res.reason)
        log("body     : %s" % short(resp.body, 300))
        explain_http(resp.status, resp.body)
        return classify(res)

    errs = []
    content = extract_content(resp.body, errs)
    if content is None:
        res.reason = errs[0] if errs else "bad response envelope"
        res.detail = short(resp.body, 300)
        log("FAILED   : %s" % res.reason)
        return classify(res)
    res.content = content
    log("answer   : %s" % short(text_safe(content), 200))

    errs = []
    parsed = parse_plate(content, errs)
    if parsed is None:
        res.reason = errs[0] if errs else "unparseable answer"
        res.detail = short(content, 200)
        log("FAILED   : %s" % res.reason)
        return classify(res)
    res.plate, res.confidence = parsed
    ok, why = plate_looks_valid(res.plate)
    res.checks.append({"name": "plate_charset", "ok": ok, "detail": why})
    log("plate    : %s   (utf-8 %d bytes, charset %s)"
        % (plate_display(res.plate),
           len(res.plate.encode("utf-8")), why))
    log("conf     : %.2f" % res.confidence)

    exit_code = classify(res)
    log("class    : %s" % res.classification)
    if res.classification == "accepted":
        log("shm write: plate=%s confidence=%.2f result_source=1 result_valid=1"
            % (res.plate, res.confidence))
        if res.confidence < TRIGGER_CONF:
            log("note     : below trigger_conf %.2f -> the board would also try "
                "the cloud (this call IS the cloud)" % TRIGGER_CONF)
    elif res.classification == "unreadable":
        log("core1    : clear cloud_pending, no result written (degrade)")
    res.elapsed_ms = int((time.time() - t0) * 1000)
    return exit_code


def strip_image_b64(payload):
    """Copy of the payload with the base64 blob shortened for printing."""
    out = json.loads(json.dumps(payload))
    for msg in out.get("messages", []):
        content = msg.get("content")
        if isinstance(content, list):
            for part in content:
                if part.get("type") == "image_url":
                    url = part["image_url"]["url"]
                    head = url[:60]
                    part["image_url"]["url"] = "%s...<%d chars total>" % (
                        head, len(url))
    return out


def explain_http(status, body):
    hints = {
        400: "bad request - model name or message format",
        401: "unauthorized - wrong/expired api key",
        402: "payment required - account balance",
        403: "forbidden - free quota exhausted or model not opened "
             "(disable 'free tier only' or add funds, then open the model)",
        404: "wrong endpoint path, or model name does not exist",
        429: "rate limited - retry later",
    }
    if status in hints:
        log("hint     : %s" % hints[status])
    elif status >= 500:
        log("hint     : server side error, retry later (Core1 retries once)")

    lower = body.lower()
    if "model_not_supported" in lower or "unsupported model" in lower:
        for family, what in NON_CHAT_FAMILIES:
            if family in lower:
                log("hint     : '%s' is a %s model - it cannot answer "
                    "chat/completions image requests" % (family, what))
                break
        log("hint     : use a vision model, e.g. qwen3-vl-plus / qwen-vl-max "
            "(see --mode models)")
    if '"code":"AllocationQuota.FreeTierOnly"' in body.replace(" ", ""):
        log("hint     : 控制台 / console -> 关闭 \"仅使用免费额度\" 或充值后重试")
    if "access_denied" in lower and "model" in lower:
        log("hint     : 该模型未开通 / model not opened: 在百炼「模型广场」点开通")


def mode_models(args, cfg, res):
    """Probe the picture capable model variants (no image, tiny request)."""
    res.mode = "models"
    res.base = cfg["base"]
    res.base_src = cfg["base_src"]
    res.key_src = cfg["key_src"]
    res.key_mask = mask_key(cfg["key"])
    if not cfg["key"]:
        log("ERROR: no api key")
        return EXIT_USAGE
    log("endpoint : %s" % res.base)
    log("key      : %s from %s" % (res.key_mask, res.key_src))
    log("model    : %s" % (args.model or DEFAULT_MODEL))
    results = []
    for name, note in MODELS + TEXT_MODELS:
        payload = {
            "model": name,
            "messages": [{"role": "user", "content": "Reply with: ok"}],
            "max_tokens": 4,
            "temperature": 0,
        }
        resp = http_post(res.base, cfg["key"], payload, args.timeout)
        if resp.status == 200:
            verdict = "OK"
        elif resp.status == 403:
            verdict = "403 quota/access"
        elif resp.status == 404:
            verdict = "404 not found"
        elif resp.status < 0:
            verdict = "network error"
        else:
            verdict = "http %d" % resp.status
        results.append({"model": name, "status": resp.status,
                        "verdict": verdict, "note": note, "ms": resp.ms})
        log("%-22s %-18s %6d ms  %s" % (name, verdict, resp.ms, note))
    res.checks = results
    usable = [r for r in results if r["status"] == 200]
    res.api_code = "ps"
    log("ps       : all probes used a text-only body; vision needs "
        "qwen-vl-* and a valid quota")
    if not usable:
        # exit code stays meaningful: 0 = at least one model answered
        res.classification = "failed"
        res.reason = "no model answered (see the table above)"
        return EXIT_FAILED
    if not any(r["status"] == 200 and "vl" in r["model"] for r in usable):
        log("WARN     : no vision model answered - image calls will fail")
    res.classification = "info"
    return EXIT_ACCEPTED


# --------------------------------------------------------------------------
# offline self test (no network): fake transport in-process


class FakeHTTPServer(object):
    """One-shot http.server bound to 127.0.0.1 serving the canned answer.

    Started through a file:// URL base: plain http.server has no TLS, but the
    production code path (urllib POST + headers + status/body handling) is the
    same object, so the HTTP layer itself is still exercised end to end.
    """

    def __init__(self, status, body_text):
        self.status = status
        self.body_text = body_text
        self.seen = []
        self.httpd = None
        self.thread = None
        self.url = ""

    def start(self):
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, fmt, *a):
                pass

            def do_POST(self):
                n = int(self.headers.get("Content-Length") or 0)
                raw = self.rfile.read(n)
                outer.seen.append({
                    "path": self.path,
                    "headers": dict(self.headers.items()),
                    "raw": raw,
                })
                try:
                    data = raw.decode("utf-8")
                except UnicodeDecodeError:
                    data = ""
                if outer.status >= 400:
                    payload = outer.body_text
                elif "data:image" in data or '"image_url"' in data:
                    payload = outer.body_text
                else:
                    payload = json.dumps({
                        "id": "chatcmpl-selftest",
                        "object": "chat.completion",
                        "model": "self-test",
                        "choices": [{"index": 0, "finish_reason": "stop",
                                     "message": {"role": "assistant",
                                                 "content": "ok"}}],
                    })
                blob = payload.encode("utf-8")
                self.send_response(outer.status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(blob)))
                self.end_headers()
                self.wfile.write(blob)

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.httpd.serve_forever,
                                       kwargs={"poll_interval": 0.05})
        self.thread.daemon = True
        self.thread.start()
        self.url = "http://127.0.0.1:%d/v1/chat/completions" % \
            self.httpd.server_address[1]
        return self.url

    def stop(self):
        if self.httpd:
            self.httpd.shutdown()
            self.httpd.server_close()
        if self.thread:
            self.thread.join(timeout=2)


def mode_selftest(args, cfg, res):
    """Offline acceptance of the parser + request contract (~40 checks)."""
    res.mode = "selftest"
    checks = []

    def check(name, cond, detail=""):
        checks.append({"name": name, "ok": bool(cond), "detail": str(detail)})
        return bool(cond)

    # --- 1. request construction -----------------------------------------
    body = build_request_body("qwen-vl-max", DEFAULT_PROMPT, "AAAA", True)
    check("body.model", body["model"] == "qwen-vl-max")
    check("body.max_tokens==64", body["max_tokens"] == 64)
    check("body.temperature==0", body["temperature"] == 0)
    check("body.one_message", len(body["messages"]) == 1)
    content = body["messages"][0]["content"]
    check("content.parts==2", len(content) == 2)
    check("content[0].type==text", content[0]["type"] == "text")
    check("content[0].text==prompt", content[0]["text"] == DEFAULT_PROMPT)
    check("content[1].type==image_url", content[1]["type"] == "image_url")
    check("image_url.prefix",
          content[1]["image_url"]["url"].startswith("data:image/jpeg;base64,"))
    check("image_url.payload",
          content[1]["image_url"]["url"].endswith(",AAAA"))
    png_body = build_request_body("m", "p", "AAAA", False)
    check("png.mime",
          "data:image/png;base64," in
          png_body["messages"][0]["content"][1]["image_url"]["url"])
    noimg = build_request_body("m", "p", "", True)
    check("no_image.parts==1", len(noimg["messages"][0]["content"]) == 1)

    # --- 2. envelope parsing ---------------------------------------------
    good = json.dumps({"choices": [{"message": {"content": "hello"}}]})
    errs = []
    check("envelope.ok", extract_content(good, errs) == "hello", errs)
    fenced = json.dumps({"choices": [{"message": {
        "content": "```json\n{\"plate\":\"京A12345\",\"confidence\":0.9}\n```"}}]})
    got = extract_content(fenced, [])
    check("envelope.fence_stripped", got == '{"plate":"京A12345","confidence":0.9}',
          got)
    errs = []
    check("envelope.api_error_is_none",
          extract_content('{"error":{"message":"boom","code":"X"}}', errs) is None)
    check("envelope.api_error_text",
          errs and errs[0].startswith("api error: boom"), errs)
    check("envelope.api_error_code", len(errs) > 1 and errs[1] == "X", errs)
    errs = []
    check("envelope.not_json",
          extract_content("<html>502</html>", errs) is None and
          errs[0] == "bad response envelope", errs)
    errs = []
    check("envelope.no_choices",
          extract_content('{"object":"chat.completion"}', errs) is None and
          errs[0] == "no choices in response", errs)
    errs = []
    check("envelope.empty_content",
          extract_content('{"choices":[{"message":{"content":""}}]}', errs) is None,
          errs)
    parts = json.dumps({"choices": [{"message": {"content": [
        {"type": "text", "text": "ab"}, {"type": "text", "text": "cd"}]}}]})
    check("envelope.content_parts", extract_content(parts, []) == "abcd")

    # --- 3. balanced brace scan -------------------------------------------
    check("brace.simple", find_json_object('x {"a":1} y') == '{"a":1}')
    check("brace.nested", find_json_object('{"a":{"b":2}}') == '{"a":{"b":2}}')
    check("brace.brace_in_string",
          find_json_object('{"a":"}{"}') == '{"a":"}{"}')
    check("brace.escaped_quote",
          find_json_object('{"a":"\\"}"}') == '{"a":"\\"}"}')
    check("brace.truncated_is_none", find_json_object('{"a":1') is None)
    check("brace.no_object_is_none", find_json_object("plain text") is None)
    check("brace.prose_around",
          find_json_object('Sure! {"plate":"X"} hope it helps') == '{"plate":"X"}')

    # --- 4. plate parsing -------------------------------------------------
    plate, conf = parse_plate('{"plate":"京A12345","confidence":0.93}', [])
    check("plate.value", plate == "京A12345", plate)
    check("plate.conf", abs(conf - 0.93) < 1e-9, conf)
    plate, conf = parse_plate('{"plate":" 沪 B 88888 ","confidence":"0.8"}', [])
    check("plate.spaces_removed", plate == "沪B88888", plate)
    check("plate.conf_from_string", conf == 0.8, conf)
    # models answer "川A·88888" / "川A-88888"; core0 matches byte for byte
    for raw in ("川A·88888", "川A•88888", "川A-88888", "川A 88888",
                "川A.88888", "川A\n88888"):
        got, _c = parse_plate('{"plate":"%s","confidence":0.9}'
                              % raw.replace("\n", "\\n"), [])
        check("plate.separator_removed[%s]" % text_safe(raw),
              got == "川A88888", got)
    plate, conf = parse_plate('{"plate":"","confidence":0}', [])
    check("plate.empty_means_unreadable", plate == "" and conf == 0.0)
    plate, conf = parse_plate('{"plate":"京A12345","confidence":95}', [])
    check("plate.conf_clamped_high", conf == 1.0, conf)
    plate, conf = parse_plate('{"plate":"京A12345","confidence":-3}', [])
    check("plate.conf_clamped_low", conf == 0.0, conf)
    # "plate" wins when present; the aliases are a last resort for providers
    # that name the field differently.
    plate, conf = parse_plate('{"plate":"京A1","number":"粤B00001"}', [])
    check("plate.preferred_over_alias", plate == "京A1", plate)
    plate, conf = parse_plate('{"number":"粤B00001","confidence":0.7}', [])
    check("plate.alias", plate == "粤B00001", plate)
    long_plate, _ = parse_plate(
        '{"plate":"%s","confidence":1}' % ("A" * 40), [])
    check("plate.truncated_to_15_bytes", len(long_plate.encode("utf-8")) <= 15,
          len(long_plate.encode("utf-8")))
    cn_plate, _ = parse_plate('{"plate":"京ABCDEFGHIJKLMNOP","confidence":1}', [])
    check("plate.utf8_boundary_safe",
          len(cn_plate.encode("utf-8")) <= 15 and not cn_plate.endswith("\ufffd"),
          cn_plate)
    errs = []
    check("plate.no_json", parse_plate("no json here", errs) is None and
          errs[0] == "no json object in answer", errs)
    errs = []
    check("plate.bad_json", parse_plate("{plate:1}", errs) is None, errs)
    check("plate.json_array_is_none", parse_plate("[1,2]", []) is None)

    # --- 5. live HTTP path against the in-process fake server -------------
    answer = json.dumps({"choices": [{"message": {
        "content": '{"plate":"粤B12345","confidence":0.88}'}}]})
    srv = FakeHTTPServer(200, answer)
    url = srv.start()
    try:
        payload = build_request_body("qwen-vl-max", DEFAULT_PROMPT,
                                     base64.b64encode(b"x" * 4096).decode(),
                                     True)
        resp = http_post(url, "sk-selftest-not-a-real-key", payload, 10)
        check("http.status_200", resp.status == 200, resp.status)
        # json.dumps escapes non-ASCII by default, so compare decoded values
        check("http.body_read",
              "粤B12345" in json.loads(resp.body)["choices"][0]["message"]
              ["content"], short(resp.body, 80))
        check("http.ms_recorded", resp.ms >= 0, resp.ms)
        check("http.request_seen", len(srv.seen) == 1, len(srv.seen))
        if srv.seen:
            req = srv.seen[0]
            hdr = {k.lower(): v for k, v in req["headers"].items()}
            check("http.header.content_type",
                  hdr.get("content-type") == "application/json",
                  hdr.get("content-type"))
            check("http.header.auth_bearer",
                  hdr.get("authorization") ==
                  "Bearer sk-selftest-not-a-real-key", hdr.get("authorization"))
            check("http.header.user_agent",
                  hdr.get("user-agent", "").startswith("park-cloud-test/"),
                  hdr.get("user-agent"))
            sent = json.loads(req["raw"].decode("utf-8"))
            check("http.sent.model", sent["model"] == "qwen-vl-max")
            check("http.sent.max_tokens", sent["max_tokens"] == 64)
            check("http.sent.image_b64_len",
                  len(sent["messages"][0]["content"][1]["image_url"]["url"]) >
                  4000)
            check("http.sent.utf8_prompt",
                  sent["messages"][0]["content"][0]["text"] == DEFAULT_PROMPT)
        errs = []
        content = extract_content(resp.body, errs)
        parsed = parse_plate(content, errs) if content else None
        check("http.roundtrip_plate",
              parsed is not None and parsed[0] == "粤B12345", parsed)
        check("http.roundtrip_conf",
              parsed is not None and abs(parsed[1] - 0.88) < 1e-9)
        # request bytes match what the log reported
        check("http.sent_bytes", resp.sent_bytes == len(
            json.dumps(payload, ensure_ascii=False).encode("utf-8")))

        # error envelope with HTTP 403 (the real DashScope free-quota answer)
        err_body = json.dumps({"error": {
            "message": "Free quota exhausted.", "type": "AllocationQuota",
            "code": "AllocationQuota.FreeTierOnly"}})
        srv.stop()
        srv = FakeHTTPServer(403, err_body)
        url = srv.start()
        resp = http_post(url, "sk-selftest", payload, 10)
        check("http.status_403", resp.status == 403, resp.status)
        errs = []
        content = extract_content(resp.body, errs)
        check("http.403_is_api_error", content is None and
              errs[0].startswith("api error: Free quota"), errs)
        check("http.403_code", len(errs) > 1 and
              errs[1] == "AllocationQuota.FreeTierOnly", errs)
    finally:
        srv.stop()

    # --- 6. network failure classification --------------------------------
    resp = http_post("http://127.0.0.1:1/v1/chat/completions", "k",
                     {"model": "m"}, 2)
    check("net.refused_is_negative", resp.status < 0, resp.status)
    check("net.refused_has_reason", bool(resp.error), resp.error)

    # --- 7. image sniffing on the shipped sample --------------------------
    sample = os.path.join(HERE, DEFAULT_IMAGE)
    if os.path.isfile(sample):
        kind, size = sniff_image(sample)
        check("image.sample_kind", kind == "jpeg", kind)
        w, h = image_dimensions(sample, kind)
        check("image.sample_dims", w > 0 and h > 0, "%dx%d" % (w, h))
        check("image.sample_size", size > 1000, size)
    else:
        check("image.sample_missing", False, sample)

    # --- 8. plate whitelist ------------------------------------------------
    check("valid.ok", plate_looks_valid("粤B12345")[0])
    check("valid.short", not plate_looks_valid("粤B1")[0])
    check("valid.empty", not plate_looks_valid("")[0])
    check("valid.bad_char", not plate_looks_valid("粤B12-45")[0])

    # --- 9. key source / privacy rules ------------------------------------
    # sample_key.txt is uploaded to GitHub, so its "sk-xxx" placeholder must be
    # refused and the real key must come from key.txt.
    real_sample = os.path.join(HERE, SAMPLE_FILE)
    if os.path.isfile(real_sample):
        check("priv.public_sample_key_refused",
              parse_sample_key(real_sample) is None,
              short(open(real_sample, encoding="utf-8",
                         errors="replace").read(), 60))
        check("priv.public_sample_base_usable",
              parse_sample_base(real_sample) == DEFAULT_BASE,
              parse_sample_base(real_sample))
    else:
        check("priv.public_sample_present", False, real_sample)

    tmpdir = selftest_tmpdir()
    pub = os.path.join(tmpdir, SAMPLE_FILE)
    priv = os.path.join(tmpdir, "key.txt")
    keys = [priv]
    try:
        with open(pub, "w", encoding="utf-8") as fh:
            fh.write('client = OpenAI(\n'
                     '    api_key="sk-xxx",\n'
                     '    base_url="https://example.invalid/v1"\n)\n')
        with open(priv, "w", encoding="utf-8") as fh:
            fh.write('api_key="sk-privateselftest0000000001"\n'
                     'base_url="https://example.invalid/v1"\n')
        saved = (globals()["HERE"], globals()["SAMPLE_FILE"],
                 globals()["KEY_FILE"], globals()["DEFAULT_BASE"])
        globals()["HERE"] = tmpdir
        globals()["SAMPLE_FILE"] = SAMPLE_FILE
        globals()["KEY_FILE"] = "key.txt"
        globals()["DEFAULT_BASE"] = saved[3]
        try:
            cfg = load_config(build_parser().parse_args(["--mode", "test"]))
            check("priv.key_from_private_file",
                  cfg["key"] == "sk-privateselftest0000000001", cfg["key_src"])
            check("priv.key_src_marks_private",
                  "private" in cfg["key_src"], cfg["key_src"])
            check("priv.base_from_key_file",
                  cfg["base"] == "https://example.invalid/v1/chat/completions",
                  cfg["base"])
            os.remove(priv)
            cfg2 = load_config(build_parser().parse_args(["--mode", "test"]))
            # only the public sample is left: its "sk-xxx" must not become a key
            check("priv.placeholder_never_used", cfg2["key"] == "", cfg2["key"])
            check("priv.base_falls_back_to_sample",
                  cfg2["base"] == "https://example.invalid/v1/chat/completions",
                  cfg2["base"])
        finally:
            (globals()["HERE"], globals()["SAMPLE_FILE"],
             globals()["KEY_FILE"], globals()["DEFAULT_BASE"]) = saved
    finally:
        # remove the whole scratch dir (both fake config files and the dir)
        try:
            shutil.rmtree(tmpdir)
        except OSError:
            for k in keys:
                try:
                    os.remove(k)
                except OSError:
                    pass
            try:
                os.rmdir(tmpdir)
            except OSError:
                pass
    # --- 10. key is never leaked into the result --------------------------
    if os.path.isfile(os.path.join(HERE, KEY_FILE)):
        real_cfg = load_config(build_parser().parse_args(["--mode", "test"]))
        if real_cfg["key"]:
            blob = json.dumps(res.as_dict(), ensure_ascii=False)
            check("priv.real_key_absent_from_result",
                  real_cfg["key"] not in blob and "sk-" not in
                  blob.replace(res.key_mask, ""),
                  res.key_mask)
            check("priv.mask_hides_key",
                  res.key_mask != real_cfg["key"] and "..." in res.key_mask,
                  res.key_mask)
        else:
            check("priv.real_key_readable", False, real_cfg["key_src"])

    failed = [c for c in checks if not c["ok"]]
    res.checks = checks
    for c in checks:
        if not c["ok"]:
            log("FAIL %s %s" % (c["name"], c["detail"]))
    log("checks   : %d total, %d failed" % (len(checks), len(failed)))
    if failed:
        res.classification = "failed"
        res.reason = "%d self test check(s) failed" % len(failed)
        return EXIT_FAILED
    res.classification = "accepted"
    log("RESULT   : all offline self tests passed (no network used)")
    return EXIT_ACCEPTED


# --------------------------------------------------------------------------
# entry point


def write_raw(path, text):
    try:
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(text)
        log("raw body : %s (%d bytes)" % (ascii_fold(path), len(text)))
    except OSError as exc:
        log("WARN: cannot write %s: %s" % (ascii_fold(path), exc))


def write_json(path, res):
    try:
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(res.as_dict(), fh, ensure_ascii=False, indent=2)
        log("json out : %s" % ascii_fold(path))
    except OSError as exc:
        log("WARN: cannot write %s: %s" % (ascii_fold(path), exc))


def build_parser():
    p = argparse.ArgumentParser(
        prog="cloud_api_test.py",
        description="Cloud fallback API test (OpenAI compatible "
                    "/chat/completions, same contract as Core1 park_ui).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="exit codes: 0 accepted / 1 failed / 2 unreadable / 3 usage")
    p.add_argument("--mode", default="recognize",
                   choices=["recognize", "test", "info", "selftest", "models"],
                   help="recognize=image round trip (default), test=text only, "
                        "info=image report, selftest=offline checks, "
                        "models=probe model names")
    p.add_argument("--image", default=None, help="image file (default %s)"
                   % DEFAULT_IMAGE)
    p.add_argument("--model", default=None,
                   help="model name (default %s)" % DEFAULT_MODEL)
    p.add_argument("--base", default=None,
                   help="endpoint URL, overrides sample_key.txt")
    p.add_argument("--key", default=None, help="api key inline (avoid: shell "
                   "history); prefer the private key.txt or $%s" % KEY_ENV)
    p.add_argument("--key-file", default=None,
                   help="alternative private key file (default key.txt)")
    p.add_argument("--prompt", default=None, help="override the OCR prompt")
    p.add_argument("--timeout", type=int, default=30,
                   help="HTTP timeout seconds (default 30; the board uses 5)")
    p.add_argument("--json-out", default=None, help="write a JSON result file")
    p.add_argument("--raw", default=None, help="write the raw response body")
    p.add_argument("--show-body", action="store_true",
                   help="print the outgoing JSON (image truncated)")
    p.add_argument("-v", "--verbose", action="store_true")
    return p


def main(argv=None):
    global VERBOSE
    args = build_parser().parse_args(argv)
    VERBOSE = args.verbose
    cfg = load_config(args)
    res = Result()
    res.base = cfg["base"]
    res.base_src = cfg.get("base_src", "")
    res.model = cfg["model"]
    res.model_src = cfg.get("model_src", "")
    res.key_src = cfg["key_src"]
    res.key_mask = mask_key(cfg["key"]) if cfg["key"] else "(no key)"

    log("cloud api test (project EdgeParking step 7 / P7-01)")
    t0 = time.time()
    try:
        if args.mode == "info":
            code = mode_info(args, cfg, res)
        elif args.mode == "test":
            code = mode_test(args, cfg, res)
        elif args.mode == "models":
            code = mode_models(args, cfg, res)
        elif args.mode == "selftest":
            code = mode_selftest(args, cfg, res)
        else:
            code = mode_recognize(args, cfg, res)
    except KeyboardInterrupt:
        log("interrupted by user")
        code = EXIT_FAILED
        res.classification = "failed"
        res.reason = "interrupted"
    if not res.elapsed_ms:
        res.elapsed_ms = int((time.time() - t0) * 1000)
    log("done     : %s, exit=%d, %d ms"
        % (res.classification or "n/a", code, res.elapsed_ms))
    if args.json_out:
        write_json(args.json_out, res)
    return code


if __name__ == "__main__":
    sys.exit(main())
