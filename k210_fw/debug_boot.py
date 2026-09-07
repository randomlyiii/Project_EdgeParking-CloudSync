# -*- coding: utf-8 -*-
"""调试版 main.py（放 flash 里，上电自动运行，逐步骤打印）——定位"白屏/卡死"发生在哪一步。
重要：本版【不】摘 REPL（不调 os.dupterm），所以 CanMV IDE 能连上用串口终端看到输出。
跑完会打印 DBG:DONE 或某一步的 fail；如果打印停在某一行不再往下，就是那一步卡死。
"""
import gc
import machine
import sensor
import lcd


def P(m):
    print("DBG:" + m)


P("start")
try:
    sensor.reset()
    P("sensor reset ok")
except Exception as e:
    P("sensor reset FAIL " + repr(e))
    raise SystemExit
try:
    sensor.set_pixformat(sensor.RGB565)
    P("pixformat ok")
except Exception as e:
    P("pixformat FAIL " + repr(e))
try:
    sensor.set_framesize(sensor.QQVGA)
    P("framesize QQVGA ok")
except Exception as e:
    P("framesize FAIL " + repr(e))
try:
    sensor.run(1)
    P("sensor run ok")
except Exception as e:
    P("sensor run FAIL " + repr(e))

P("lcd init")
try:
    lcd.init()
    lcd.clear(lcd.BLACK)
    P("lcd init+clear ok")
except Exception as e:
    P("lcd init FAIL " + repr(e))
try:
    img = sensor.snapshot()
    P("snapshot " + ("None" if img is None else "ok"))
    lcd.display(img)
    P("lcd display ok")
except Exception as e:
    P("snapshot/lcd FAIL " + repr(e))

P("UART(1) pins test")
try:
    u = machine.UART(1, 921600, 8, None, 1)
    P("UART1 open ok")
except Exception as e:
    P("UART1 open FAIL " + repr(e))

P("cdc dupterm test (may hang the board if unsupported)")
try:
    import os
    os.dupterm(None, 0)
    P("dupterm(0) ok")
except Exception as e:
    P("dupterm(0) FAIL " + repr(e))
try:
    os.dupterm(None, 1)
    P("dupterm(1) ok")
except Exception as e:
    P("dupterm(1) FAIL " + repr(e))
try:
    u0 = machine.UART(0, 921600, 8, None, 1)
    P("UART0 open ok")
except Exception as e:
    P("UART0 open FAIL " + repr(e))

P("DONE")
