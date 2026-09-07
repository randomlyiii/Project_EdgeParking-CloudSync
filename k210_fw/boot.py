# -*- coding: utf-8 -*-
"""boot.py —— K210 上电最先执行（CanMV MicroPython）。

保持极简：业务入口在 main.py（自启）。如需在 REPL 下手动 import main 测试，
不要在这里改入口；提频/其余引导动作如需可在此加（K210 默认 400MHz，一般不动）。
"""
import gc

gc.collect()
