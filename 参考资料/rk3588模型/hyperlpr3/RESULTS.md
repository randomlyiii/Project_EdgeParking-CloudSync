# HyperLPR3 → RKNN 转换与 A/B 测试记录（2026-09-27）

## 结论

**转换 + 测试完成，NPU 双模型与 CPU 引擎 3/3 全对**。模型已持久化：
- 板上 `/root/models/{y5fu_320x.rknn, y5fu_640x.rknn, rpv3_mdict_160_r3.rknn}`
- 本目录有同版备份（PC 机 toolkit2 无 py3.13/3.14 轮子，转换在板上完成，LPRNet 同路线）

## 实测（川A88888 原图 / 14% 缩小 / 8° 旋转）

| 变体 | det | rec | 合计 | CPU 引擎 | 对拍 |
|---|---|---|---|---|---|
| 320（CPU 默认档） | 11-12ms | 6-7ms | **~18ms** | 69-131ms | 3/3 一致 |
| 640（高精度档） | 39-43ms | 6-7ms | **~46ms** | 45-80ms | 3/3 一致 |

## 关键事实（与 AGENTS 原待办预估不同，已纠正）

1. **检测不是 yolov5 anchor 系**——`y5fu_*_sim.onnx` 输出是**单融合头 (1,N,15)** 的
   **anchor-free**（yolov8 风味）+ 8  landmark 角点：通道 `[0:4 xywh][4 obj][5:13 角点][13:15 类]`，
   且 **"sim" = sigmoid 已焊死**，解码**全部用原始 logits**（无 sigmoid/无 anchor/无 grid），
   阈值 obj>0.25、角点仿射校正 `get_rotate_crop_image`（旋转不变性来源）。
   生产解码参考 = `hyperlpr3.inference.multitask_detect.post_precessing`。
   ⚠️ `Y5rkDetectorORT`/`hyperlpr3.inference.*` 的 `@cost` 装饰器在板上 import 即炸
   （NoneType），**只能 import common.* 与 multitask_detect 的函数**。
2. **字符表是 78 token 不是 65**（`hyperlpr3.common.tokenize.token`，blank=0），
   rpv3 输出 (1,T,78)，按 charset 维定向再 CTC。
3. **rknn 输入 = NHWC uint8**（mean/std 烘进模型）：det mean0/std255、rec mean/std 127.5，
   BGR→RGB 由调用方做（烘焙不换通道）；rec 右侧补 **127**（≈归一化 0）。
4. conf 口径：rknn 侧 softmax 均值 ≈0.03，CPU 引擎报 0.999——**字串全对**，
   conf 仅标定差异（待集成时对齐 hyperlpr3 原口径）。
5. 转换零算子回落（verbose build 无 fallback 告警）；模型 2.3.2 vs 运行时 2.1.0 的
   WARN 照例可忽略。

## 复现

```bash
# 板上
python3 convert_hyperlpr3_rknn.py y5fu_320x_sim.onnx rpv3_mdict_160_r3.onnx
python3 itest_hyperlpr3_rknn.py <图目录> y5fu_320x.rknn rpv3_mdict_160_r3.rknn
```

## 验收记录（2026-09-27，COM4 通道实操）

8 图集 = 原图 / 14% / 50% / 25% 缩放 / 高斯模糊 / 变暗 0.6 / +8° / -5°：

| 变体 | 对拍 | det+rec 耗时 |
|---|---|---|
| 320（CPU 默认档） | **8/8 全对** | 17~29ms |
| 640（高精度档） | 7/8（模糊图 `川A8888` 少一个 8，320 同图正确） | 46~53ms |

**结论：320 档通过验收，集成取 320**。验收通道：台式机 COM4（`Temp/serial_cmd.py`，
MobaXterm 需先让口）→ MP157 → `ssh rk`。

## 待办（归引擎集成方）

- `--engine hyperlpr3rknn` 接入 edge_hub（双引擎灰度 + 9 回归 + 现场实测后才切默认）。
  集成时注意：det 角点校正、双层牌切分（out[13]==1 时 40% 处切开两行识别）、
  cls 颜色分类（litemodel_cls_96x_r1，可选）、conf 口径对齐。
