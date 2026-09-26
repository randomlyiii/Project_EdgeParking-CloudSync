# -*- coding: utf-8 -*-
"""K210 固件【图像采集上行】—— 只负责把画面拍下来发给上位机。

角色（2026-09-21 定）：本地 KPU 识别已移除，识别主责移到上位机 **rk3588 | MP157**。
本文件 = 相机采集 + 软件方向修正 + JPEG 压缩 + 串口/console 上行图片帧 + 心跳/统计，
**不再加载 SD 卡 / KPU 模型 / 车牌识别**（旧识别相关文件见 `k210_fw/tmp_kpu_old/`）。

保存到设备：文件名必须是 `main.py`（`/flash/main.py` 开机自启）。
帧协议见 docs/protocols.md §2；图片走 cls=`console`（base64 文本行，USB 必通）或
二进制帧链路（0x01 分块 + 0x02 尾）。

调参入口 = 下方「常量区」：相机分辨率/帧率/JPEG 质量、方向修正、发图频率、心跳节奏。
"""
import gc
import machine          # _open_uart 用（LINK="uart" 时）
import sys
import uos              # _stat_size / _fw_ver 用
import utime

# CanMV IDE「运行」只投递当前文件；把常见模块根补进 sys.path（无害，不存在则忽略）。
for _p in ("/flash", "/sd"):
    if _p not in sys.path:
        sys.path.append(_p)

# ============================ 常量区 ============================
# ---- 相机 / JPEG ----
# 实测(固件1.0.4)：set_pixformat(sensor.JPEG) 报 "Pixel format is not supported!" ——
# 本固件无硬件 JPEG，只能用软件 compress()（CPU 时间≈像素数，是 fps 瓶颈）。
CAM_W, CAM_H = 320, 240   # QVGA（帧率优先可临时改 160,120）
JPEG_HW = False           # 硬件 JPEG 本固件不支持（实测报错），保持 False 走软件压缩
JPEG_QUALITY = 40         # 软件压缩质量。上位机识别用图：画质不够就调高（代价是帧率↓）
LCD_PREVIEW = False       # 板载屏本地预览。置 1 = 相机会自动降到 QQVGA(160x120)
                          # ⛔ 不要尝试"先开 LCD 再 deinit 让给相机"：实测配相机会**硬故障**。
CAM_FRAMEBUFFERS = 1      # 相机器帧缓冲数量（QVGA RGB565 一帧 ~150KB 系统堆，取 1 最省）
SENSOR_RUN = 1            # 置 0 = 完全不碰 sensor.run(1)/skip_frames（排查假异常用，正常保持 1）

# ---- 方向修正（两层，实测传感器层无效，默认走软件层）----
# 软件层(img.replace(hmirror/vflip))：直接改像素，**必然生效**，上行 JPEG 方向一致。
# 症状是水平镜像（左右反）→ 开 hmirror；竖直反 → 开 vflip。改完以 [CFG] 行对账。
CAM_VFLIP = False         # 传感器层（本固件无效，保留开关备用）
CAM_HMIRROR = False
CAM_SW_VFLIP = False      # 软件层：垂直镜像（上下反）→ 关
CAM_SW_HMIRROR = True     # 软件层：水平镜像（左右反）→ 开（修正当前症状）

# ---- 识别取景框（2026-09-27，固件级裁剪，治帧率+准确率）----
# 只框车牌区域再编码上行：小图编码快（帧率↑）、车牌占满画面（RK 的 LPRNet
# 吃的是"车牌占满"级别的图，整帧 320x240 缩到 94x24 远处车牌必糊 → 准确率↑）。
# RK 侧收到即裁好的图，lpr_server 无需 --roi。代价：上位机 LCD 预览 = 车牌特写。
# 相对坐标 0~1：(x, y, w, h)；None = 全帧。调法：先 None 全帧跑，抓一帧看构图，
# 把车牌框换算成比例填这里，CanMV IDE「保存到设备」生效；[CFG] 行回显 crop= 对账。
CAM_CROP = None           # 例：(0.25, 0.55, 0.5, 0.30)

# ---- 上行链路 ----
# ⚠️ 实测(2026-09-07)：本固件 USB 只走 console(print) 文本；machine.UART 二进制帧到不了 USB。
# LINK="console"=走 console 打印 base64 文本行（零线、必通、较慢，QVGA 约 0.5~1s/帧）；
# LINK="uart"=板级串口 UART_ID（预留，需杜邦线）。
LINK = "console"
CONSOLE_CHUNK = 900       # console 模式：每行 base64 字符数(600→900 减行数提fps)
CONSOLE_PACE_MS = 25      # 每行间延时(丢行就调回 40)
CONSOLE_PREVIEW = 1       # 置 0 = 停发 base64 预览(不压缩、省 CPU)，串口只剩诊断行
CONSOLE_IMG_EVERY = 2     # 每 N 帧发一张图（0/1 = 每帧都发）。发图频率：识别需要更多帧就调小。
                          # 2026-09-26 起主机 = RK3588 USB CDC，带宽充裕：2 -> 上行 ~5fps；
                          # 旧值 10 是 MP157 串口时代省带宽留的，预览会显得卡。
UART_ID = 1               # LINK="uart" 时的板级串口号（预留）
UART_BAUD = 921600        # 起步值；实测误码可退回 460800
UART_RX_BUF_LEN = 4096

# ---- 心跳 / 统计 / GC ----
HEARTBEAT_MS = 1000       # 0x7E 心跳周期
GC_PERIOD_MS = 1000       # gc 周期：发图路径每帧会造几十 KB 临时垃圾（base64+切片）
STAT_PERIOD_MS = 2000     # 性能统计打印周期

# ---- 帧协议（对应 docs/protocols.md §2）----
HEADER = b"\xAA\x55"
TYPE_PREVIEW_CHUNK = 0x01   # ↑ JPEG 分块
TYPE_PREVIEW_TAIL = 0x02    # ↑ 帧尾：4B 本帧总长(LE) + 1B 用途
TYPE_HEARTBEAT = 0x7E       # ↔ 心跳/状态（1B：bit0 busy）
FRAME_KIND_PREVIEW = 0      # 0x02 用途：预览帧
CRC_POLY = 0x1021
CRC_INIT = 0x0000
PREVIEW_CHUNK = 900         # 二进制帧链路：每片 payload 字节数

# ---- GC 堆保护（2026-09-13 砖机教训：只读 + 只许抬高，绝不自动压小）----
HEAP_TUNE = 1                     # 置 0 = 连体检都不做
HEAP_TUNE_GC_MIN = 384 * 1024     # 低于此值 main.py 可能**编译不出来**（实测 248KB 挂）
HEAP_TUNE_GC_RESTORE = 512 * 1024

# 构建指纹（每改一次板端代码就 +1；`[CFG]` 开机行会打出来）。
# 为什么必须有它（2026-09-16 真机教训）：设备上 `/flash/main.py` 与我们仓库这份
# **可以不是同一个文件**——日志里对不上 build 值 = 没存进去/存的是旧版。
BUILD = "2026-09-21-imgonly"


def _log(msg):
    """cdc 模式下 print 与二进制帧共用同一条 CDC，会污染帧流（靠对端丢帧容错），
    故 cdc 模式静默；uart 模式正常打印。"""
    if LINK != "cdc":
        print(msg)


# ============================ 协议层（CRC16 + 组帧） ============================
# ---- CRC16：优先 binascii.crc_hqx（C 实现，即 XMODEM 参数：poly 0x1021、
# 不反射、无异或，init 由入参给定）；导入失败退回 256 项查表法，结果一致。
try:
    from binascii import crc_hqx as _crc_hqx_fast
except ImportError:
    _crc_hqx_fast = None

if _crc_hqx_fast is not None:
    def crc16(data):
        """CRC-16/XMODEM。校验向量：crc16(b"123456789") == 0x31C3。"""
        return _crc_hqx_fast(data, CRC_INIT)
else:
    def _build_table():
        tb = []
        for i in range(256):
            c = i << 8
            for _ in range(8):
                c = ((c << 1) ^ CRC_POLY) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
            tb.append(c)
        return tuple(tb)

    _CRC_TABLE = _build_table()

    def crc16(data):
        """CRC-16/XMODEM（查表回退实现，与 crc_hqx 结果一致）。"""
        crc = CRC_INIT
        for b in data:
            crc = ((crc << 8) & 0xFFFF) ^ _CRC_TABLE[(crc >> 8) ^ b]
        return crc


def encode_frame(type_, seq, payload=b""):
    body = (bytes([type_])
            + int(seq & 0xFFFF).to_bytes(2, "little")
            + len(payload).to_bytes(2, "little")
            + bytes(payload))
    return HEADER + body + crc16(body).to_bytes(2, "little")


# ============================ 内存/诊断小工具 ============================
_DBG_SHOWN = {}            # 一次性调试打印（避免每帧刷屏）


def _dbg_once(tag, msg):
    if tag not in _DBG_SHOWN:
        _DBG_SHOWN[tag] = True
        print("[DBG] %s: %s" % (tag, msg))


def _mem2():
    """'free=<GC堆> sysfree=<系统堆>' —— 兜底日志专用，绝不抛异常。"""
    try:
        return "free=%d sysfree=%d" % (gc.mem_free(), _sys_heap_free()[0])
    except Exception:
        return "free=?"


def _stat_size(path):
    """返回文件字节数；不存在/读不到返回 -1。"""
    try:
        import uos
        return uos.stat(path)[6]
    except Exception:
        try:
            import os
            return os.stat(path)[6]
        except Exception:
            return -1


def _maix_utils():
    """取本固件的 maix.utils（模块名是小写 `maix`，也兼容 `Maix` 老写法）。"""
    for name in ("maix", "Maix"):
        try:
            u = getattr(__import__(name), "utils", None)
            if u is not None and hasattr(u, "heap_free"):
                return u, name + ".utils"
        except Exception:
            pass
    try:
        from maix import utils as u
        return u, "maix.utils(import)"
    except Exception:
        return None, ""


def _sys_heap_free():
    """(free, 来源) —— 系统堆余量。**整体兜底**：本固件 C 层会偶发抛假异常，
    读数不许把调用方（cam_init 等）带走。"""
    try:
        u, src = _maix_utils()
        if u is None:
            return -1, src
        try:
            return u.heap_free(), src
        except Exception as e:
            return -1, "%r" % (e,)
    except Exception as e:
        return -1, "guard:%r" % (e,)


def _fw_ver():
    """固件标识：优先 `uos.uname()`（真正的 build 串），拿不到才退 `sys.version`。"""
    try:
        u = uos.uname()
        try:
            return ("%s; %s" % (u.release, u.version))[:64]
        except Exception:
            return ("; ".join(str(x) for x in u))[:64]
    except Exception:
        pass
    try:
        v = sys.version
        part = v.split("(", 1)
        head = part[0].strip()
        tail = part[1].split(")", 1)[0] if len(part) > 1 else ""
        return ("%s %s" % (head, tail)).strip()[:64]
    except Exception:
        return "unknown"


def heap_report(tag):
    """打印两个内存池的余量（GC 堆变量 / 系统堆图像缓冲）。"""
    free, src = _sys_heap_free()
    u, _ = _maix_utils()
    gch = -1
    if u is not None:
        try:
            gch = u.gc_heap_size()
        except Exception:
            pass
    print("[MEM] %-22s gc_free=%-8d gc_heap=%-8d sys_free=%-8s (%s)"
          % (tag, gc.mem_free(), gch, free, src))


# ============================ 摄像头 ============================
def _sensor_setup(sensor, w, h):
    """按给定尺寸配置相机。返回 None=成功，否则返回异常。

    帧缓冲数量**放在 set_framesize 之前**设：`set_framesize` 才是真正分配缓冲的
    那一步（2026-09-13 板测：ENOMEM 就出在它身上），放在后面就来不及了。
    """
    fn = getattr(sensor, "set_framebuffernum", None)
    if fn is None:
        print("[MEM] this firmware has no sensor.set_framebuffernum")
    else:
        try:
            fn(CAM_FRAMEBUFFERS)
            print("[MEM] set_framebuffernum(%d) before set_framesize"
                  % CAM_FRAMEBUFFERS)
        except Exception as e:
            print("[MEM] set_framebuffernum(%d) failed: %r"
                  % (CAM_FRAMEBUFFERS, e))
    table = {(160, 120): sensor.QQVGA, (320, 240): sensor.QVGA,
             (640, 480): sensor.VGA}
    try:
        sensor.set_pixformat(sensor.RGB565)
        sensor.set_framesize(table.get((w, h), sensor.QVGA))
        # ⚠️ 方向必须放在 set_framesize **之后**（2026-09-14 改）：set_framesize 会重写
        #    传感器窗口/寄存器，放前面会被覆盖。
        if CAM_VFLIP:
            sensor.set_vflip(True)
        if CAM_HMIRROR:
            sensor.set_hmirror(True)
        print("[CAM] %dx%d RGB565 sensor_vflip=%s sensor_hmirror=%s"
              " sw_vflip=%s sw_hmirror=%s"
              % (w, h, CAM_VFLIP, CAM_HMIRROR, CAM_SW_VFLIP, CAM_SW_HMIRROR))
        return None
    except Exception as e:
        return e


def cam_init():
    """开 LCD/相机：**每一步打系统堆余量**，内存不够就降一档（绝不 deinit 再重试）。

    只发图后不再加载模型，系统堆只装相机+LCD，宽裕很多；仍保留"QVGA → QQVGA"兜底。
    ⛔ **绝不能有 `lcd.deinit()` 这一手**：实测"放掉 LCD 再重配相机"会**硬故障**
       （`EPC 0x8006d722`）。LCD 开关进函数前就定死，之后只降相机尺寸。
    """
    import sensor
    global CAM_W, CAM_H
    free0 = _sys_heap_free()[0]
    print("[MEM] cam_init: sys_free=%d before anything" % (free0,))
    if LCD_PREVIEW:
        try:
            import lcd
            lcd.init()
            lcd.clear(lcd.RED)
        except Exception as e:
            print("[WARN] lcd init failed:", e)
        free1 = _sys_heap_free()[0]
        print("[MEM] cam_init: sys_free=%d after lcd.init (panel buffer took %d B;"
              " QVGA camera needs ~%d B, so expect ENOMEM and a QQVGA fallback)"
              % (free1, free0 - free1, 2 * CAM_W * CAM_H * 2))
    sensor.reset()
    # 两档就够：目标尺寸 → QQVGA 兜底。LCD 状态全程不变（见 docstring）。
    attempts = ((CAM_W, CAM_H), (160, 120))
    got = None
    for w, h in attempts:
        if got is not None:
            break

        err = _sensor_setup(sensor, w, h)
        if err is None:
            CAM_W, CAM_H = w, h
            got = (w, h)
            print("[MEM] camera %dx%d configured, sys_free=%d"
                  % (w, h, _sys_heap_free()[0]))
            break
        print("[MEM] camera %dx%d failed: %r (sys_free=%d)"
              % (w, h, err, _sys_heap_free()[0]))
    if got is None:
        print("[WARN] camera could not be configured at ANY size -> no image to send")
    try:
        if SENSOR_RUN:
            sensor.run(1)
            try:
                sensor.skip_frames(time=1500)   # 等自动曝光/白平衡稳定，避免头几帧花屏
            except Exception:
                pass
        print("CAM %dx%d" % (sensor.width(), sensor.height()))
    except Exception as e:
        print("[WARN] sensor.run failed: %r" % (e,))
    try:
        print("[MEM] cam_init: sys_free=%d after camera+LCD" % (_sys_heap_free()[0],))
    except Exception as e:
        # ⛔ 这一行曾经把整个 cam_init 掀掉（C 层假 TypeError）。日志行永远不许毁掉一个阶段。
        print("[WARN] cam_init tail print failed: %r" % (e,))


def apply_crop(img):
    """固件级取景框（CAM_CROP）：裁完再编码/上屏。裁剪失败回退全帧，绝不抛。"""
    if img is None or CAM_CROP is None:
        return img
    x = int(CAM_CROP[0] * CAM_W)
    y = int(CAM_CROP[1] * CAM_H)
    w = min(int(CAM_CROP[2] * CAM_W), CAM_W - x)
    h = min(int(CAM_CROP[3] * CAM_H), CAM_H - y)
    if w < 8 or h < 8:
        return img
    try:
        return img.crop((x, y, w, h))
    except Exception:
        return img


def capture_frame():
    """snapshot 一帧 + 软件方向校正，返回 image（未压缩）；失败返回 None。

    ⚠️ 校正放在拍摄后、一切消费之前：上行 JPEG 因此方向一致
    （本固件传感器层 set_hmirror 无效，只能软件层修，见常量区注释）。"""
    try:
        import sensor
        img = sensor.snapshot()
    except Exception:
        return None
    if img is None:
        return None
    if CAM_SW_HMIRROR:
        try:
            img.replace(hmirror=True)
        except Exception as e:
            _dbg_once("sw_hmirror", "replace failed: %r" % (e,))
    if CAM_SW_VFLIP:
        try:
            img.replace(vflip=True)
        except Exception as e:
            _dbg_once("sw_vflip", "replace failed: %r" % (e,))
    return img


def lcd_show(img):
    """板载屏本地预览。⚠️ 必须在 compress 之前调——compress() 原地改写 img。"""
    if not LCD_PREVIEW or img is None:
        return
    try:
        import lcd
        lcd.display(img)
    except Exception:
        pass


def jpeg_from(img):
    """把已拍摄的 image 压成 JPEG 字节；失败返回 None。

    ⚠️ 失败**不许静默吞掉**：必打一次原因；也**不再**在 compress 失败时退回
    `bytes(img)`（会把 RGB565 裸像素当 JPEG 塞进流，对端只会解出垃圾帧）。
    ⚠️ 系统堆不足时自动降级：原地 resize 到 QQVGA(160x120) 再压一次（上位机画质足够）。
    """
    try:
        if img is None:
            return None
        if JPEG_HW:
            return bytes(img)
        try:
            comp = img.compress(quality=JPEG_QUALITY)
        except MemoryError:
            _dbg_once("jpeg_degrade",
                      "QVGA compress OOM -> resize QQVGA then retry")
            img.resize(CAM_W // 2, CAM_H // 2)
            comp = img.compress(quality=JPEG_QUALITY)
        if comp is None:
            _dbg_once("jpeg_none", "img.compress(quality=%d) returned None"
                      % JPEG_QUALITY)
            return None
        return bytes(comp)
    except Exception as e:
        _dbg_once("jpeg_fail", "img.compress(quality=%d) failed: %r"
                  % (JPEG_QUALITY, e))
        return None


def capture_jpeg():
    """拍一帧返回 JPEG 字节；失败返回 None。"""
    return jpeg_from(capture_frame())


# ============================ 发图链路 ============================
class PreviewStream:
    """二进制帧发图：send 为注入的帧写出回调（main 里绑 uart.write）。"""
    def __init__(self, send, chunk_payload):
        self.send = send
        self.chunk = chunk_payload
        self._seq = 0

    def send_jpeg(self, jpeg, kind=FRAME_KIND_PREVIEW):
        if not jpeg:
            return
        for off in range(0, len(jpeg), self.chunk):
            self._frame(TYPE_PREVIEW_CHUNK, jpeg[off:off + self.chunk])
        tail = len(jpeg).to_bytes(4, "little") + bytes([kind])
        self._frame(TYPE_PREVIEW_TAIL, tail)

    def _frame(self, type_, payload):
        self.send(encode_frame(type_, self._seq, payload))
        self._seq = (self._seq + 1) & 0xFFFF


class _NullUart:
    """LINK="console" 时代的占位 UART：write 丢弃、any 恒空。"""
    def write(self, *a):
        return 0

    def any(self):
        return 0

    def read(self, n=-1):
        return b""


def _open_uart():
    """数据口（LINK="uart" 时用板级串口 UART_ID/921600）。
    LINK="console" 时不需要 UART（走 print 文本），返回 _NullUart 占位。
    """
    try:
        from machine import UART
    except Exception:
        return _NullUart()
    if LINK != "uart":                      # console/其它模式不需要板级 UART
        return _NullUart()
    uid = UART_ID
    try:
        return UART(uid, UART_BAUD, 8, None, 1,
                    timeout=0, read_buf_len=UART_RX_BUF_LEN)
    except TypeError:
        try:
            return UART(uid, UART_BAUD, 8, None, 1, timeout=0)
        except Exception:
            return _NullUart()
    except Exception:
        return _NullUart()


_preview_fail_n = 0        # 预览丢帧限频上报（见 App._preview_drop）


class App:
    def __init__(self):
        self.uart = _open_uart()
        self.preview = PreviewStream(self.uart.write, PREVIEW_CHUNK)
        self.hb_last = utime.ticks_ms()
        self._seqs = {}
        self.stat_last = utime.ticks_ms()
        self.stat_frames = 0
        self.stat_jlen = 0
        self.gc_last = utime.ticks_ms()
        self.img_seq = 0                    # 发图帧计数（CONSOLE_IMG_EVERY 降频用）

    def tx(self, type_, payload=b""):
        seq = self._seqs.get(type_, 0)
        self._seqs[type_] = (seq + 1) & 0xFFFF
        self.uart.write(encode_frame(type_, seq, payload))

    def _preview_drop(self, why):
        """预览失败**限频**上报（首现 3 次 + 之后每 20 次一行）——自身绝不抛。

        它专门在"分配失败/写 CDC 失败"时被调用，所以这里连 print 都不能信。"""
        global _preview_fail_n
        try:
            _preview_fail_n += 1
            if _preview_fail_n <= 3 or _preview_fail_n % 20 == 0:
                print("[DBG] image send dropped (#%d): %s (if this repeats,"
                      " lower CONSOLE_IMG_EVERY / JPEG_QUALITY)"
                      % (_preview_fail_n, why))
        except Exception:
            pass

    def console_send_jpeg(self, jpeg):
        """零线 console 文本传输：JPEG -> base64 分行走 print（上位机解析 K2:IMG/K2:END 行）。
        ⚠️ 本固件实测 machine.UART 二进制到不了 USB；console(print) 文本可以（保真/慢）。

        ⛔ 整段**绝不许把异常抛出去**。真机现象：大帧（14280 字符 / 16 行）后串口直接
        断连，且全场没有一条 Python 报错。发图只是"把画面给上位机"：它在这里抛
        （分片格式化 / print 分配失败 / CDC 写失败），主循环就被掀掉、图片流一起停。
        所以：失败只丢这一帧 + 限频打一行，绝不中断。半帧已发出也没关系——对端
        （k210_link / rk3588 侧）按"总长必须等于 K2:END 声明的长度"校验，残帧不会当图。
        """
        try:
            import binascii
            b64 = binascii.b2a_base64(jpeg).decode().rstrip("\n")
        except Exception as e:
            self._preview_drop("at b64: %r" % (e,))
            return
        try:
            for i in range(0, len(b64), CONSOLE_CHUNK):
                print("K2:IMG:%d:%s" % (i, b64[i:i + CONSOLE_CHUNK]))
                utime.sleep_ms(CONSOLE_PACE_MS)
            print("K2:END:%d" % len(b64))
        except Exception as e:
            self._preview_drop("mid-frame at %d chars: %r" % (len(b64), e))

    def run(self):
        print("[BOOT] k210 image feeder (recognition runs on the host: rk3588|mp157)"
              " link=%s preview=%d sw_vflip=%d sw_hmirror=%d"
              % (LINK, CONSOLE_PREVIEW,
                 1 if CAM_SW_VFLIP else 0, 1 if CAM_SW_HMIRROR else 0))
        # 配置指纹（2026-09-16 教训）：设备上跑的到底是哪一版、方向/频率各是多少 ——
        # 开机第一屏全写清楚。现场标定方向就靠**这行**读回仓库（build=/sw_hmirror=）。
        print("[CFG] build=%s app=%dB cam=%dx%d conf_th=n/a img_every=%d"
              % (BUILD, _stat_size("/flash/main.py"), CAM_W, CAM_H,
                 CONSOLE_IMG_EVERY))
        print("[BOOT] firmware: %s" % _fw_ver())
        try:
            cam_init()                      # 只发图：相机是唯一大件，直接开
        except Exception as e:
            print("[WARN] cam_init failed:", e)
        print("[BOOT] entering main loop")
        while True:
            now = utime.ticks_ms()
            img = capture_frame()
            lcd_show(img)                   # 先上屏（compress 会原地改写 img）
            if img is not None:
                self.stat_frames += 1
                if CONSOLE_PREVIEW:
                    # 发图降频（CONSOLE_IMG_EVERY>1 时每 N 帧发一次），
                    # 屏幕/IDE 帧缓冲不受影响，只是串口不被 base64 刷满。
                    n = CONSOLE_IMG_EVERY if CONSOLE_IMG_EVERY and CONSOLE_IMG_EVERY > 1 else 1
                    if self.img_seq % n == 0:
                        jpeg = jpeg_from(img)
                        if jpeg:
                            self.stat_jlen = len(jpeg)
                            if LINK == "console":
                                self.console_send_jpeg(jpeg)
                            else:
                                self.preview.send_jpeg(jpeg)
                    self.img_seq += 1
            if utime.ticks_diff(now, self.hb_last) >= HEARTBEAT_MS:
                self.hb_last = now
                self.tx(TYPE_HEARTBEAT, bytes([0]))   # busy 恒 False，bit0 = 0
            if utime.ticks_diff(now, self.stat_last) >= STAT_PERIOD_MS:
                fps = self.stat_frames * 1000 // max(1, utime.ticks_diff(now, self.stat_last))
                _log("[stat] fps=%d jpeg=%dB mem_free=%d"
                     % (fps, self.stat_jlen, gc.mem_free()))
                self.stat_frames = 0
                self.stat_last = now
            if utime.ticks_diff(now, self.gc_last) >= GC_PERIOD_MS:
                self.gc_last = now
                gc.collect()
            utime.sleep_ms(2)


_APP = None                 # 当前 App 实例


def _apply_gc_heap(target):
    """把 GC 堆改成 target，并验证它**当场**生效。返回 (status, before, after)。

    status:
      "live"     —— 系统堆确实动了（划分当场换了）
      "deferred" —— 调用没报错、但系统堆一点没动：固件把生效推迟到下次上电
      "failed"   —— maix.utils 不可用，或 gc_heap_size() 直接抛异常
    """
    u, _ = _maix_utils()
    if u is None:
        return "failed", -1, -1
    before, _ = _sys_heap_free()
    try:
        u.gc_heap_size(target)
    except Exception as e:
        print("[MEM] gc_heap_size(%d) failed: %r" % (target, e))
        return "failed", before, before
    try:
        gc.collect()
    except Exception:
        pass
    after, _ = _sys_heap_free()
    return ("live" if after != before else "deferred"), before, after


def auto_tune_gc_heap():
    """GC 堆**体检**：只读 + 只许抬高，**永不自动压小**。返回 True = 需要冷启动。

    2026-09-13 定案：GC 堆与系统堆是 1:1 的真交易（同一个池子），"压过头"会让
    main.py 下次**编译不出来**（实测 248KB 时开机即 MemoryError）。⇒ 本函数只做：
    打内存底账；把**过小**的 GC 堆抬回固件默认值。系统堆够不够装相机，交给
    cam_init() 用真实数字报出来。
    """
    if not HEAP_TUNE:
        return False
    u, src = _maix_utils()
    if u is None:
        print("[MEM] maix.utils not found -> skip the heap check")
        return False
    try:
        cur = u.gc_heap_size()
    except Exception:
        cur = 0
    try:
        gc_free = gc.mem_free()
    except Exception:
        gc_free = 0
    print("[MEM] boot: sys_free=%d gc_heap=%d gc_free=%d (%s)"
          % (_sys_heap_free()[0], cur, gc_free, src))
    if not cur:
        print("[MEM] gc_heap_size() unreadable -> skip the heap check")
        return False
    if cur >= HEAP_TUNE_GC_MIN:
        # 正常路径：什么都不做。压小在这里没有任何收益，只有把板子写死的风险。
        print("[MEM] gc_heap %d B >= %d B -> fine, NOT touching it"
              % (cur, HEAP_TUNE_GC_MIN))
        return False
    # 唯一会写的一步：GC 堆小到可能编译不出 main.py ⇒ 抬回固件默认值（只赚不赔）
    print("[MEM] gc_heap %d B is BELOW %d B -> main.py may fail to COMPILE;"
          " raising it back to %d B" % (cur, HEAP_TUNE_GC_MIN, HEAP_TUNE_GC_RESTORE))
    try:
        status, before, after = _apply_gc_heap(HEAP_TUNE_GC_RESTORE)
    except Exception as e:
        print("[MEM] raising the GC heap failed: %r" % (e,))
        return False
    print("[MEM] sys_free %d -> %d (%s)" % (before, after, status))
    if status == "deferred":
        print("[BOOT] the GC heap raise is not live yet -> POWER-CYCLE the board"
              " (it persists, so this happens only once)")
        return True
    if status == "live":
        print("[MEM] GC heap raised -> keep booting")
        return False
    print("[MEM] could not raise the GC heap -> continuing as is")
    return False


def main():
    global _APP
    if auto_tune_gc_heap():
        # 只有"把过小的 GC 堆抬回去"这一种情况会返回 True；本次不装模型，
        # 但发图照常，用户断电上电一次即可。
        print("[BOOT] GC heap raise needs a cold boot")
    _APP = App()
    _APP.run()


if __name__ == "__main__":
    main()