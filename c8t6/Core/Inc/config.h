/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    config.h
  * @brief   C8T6 下位机全局配置：遮光检测 / CAN 2.0 / SG90 / OLED 参数收拢。
  *
  *          背景：参数散落在 freertos.c/bh1750.h 等多处难调，统一收进本文件。
  *          现场调参(敏感度、ID、周期)只改这里；代码 include 本文件即可。
  *          生成文件见 c8t6/实现步骤.md §5 与 c8t6/can.md。
  ******************************************************************************
  */
/* USER CODE END Header */
#ifndef __CONFIG_H
#define __CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* =========================== 检测灵敏度档位(语义参数, CAN 0x100/0x10) ===== */
/* 板级(A7/M4)只下发"档位"这个语义参数(1..5)，具体掉点阈值由本节点自己定 ——
   这就是"板子层与下位机隔离"：以后把 BH1750 换成红外/超声波/地磁，
   只改本文件这张表，Linux 侧一行代码都不用动。
   档位越高越灵敏(越容易判为"车到位")；释放阈值 = 掉点阈值 / SHADE_RELEASE_DIV(回滞)。
   BH1750 采样周期以 bh1750.h 的 BH1750_READ_PERIOD_MS(=200ms) 为准，
   状态机每周期喂一次有效 lux；去抖 = 连续 SHADE_CONFIRM_N 次满足才翻转。 */
#define SHADE_LEVEL_MIN          1u
#define SHADE_LEVEL_MAX          5u
#define SHADE_LEVEL_DEFAULT      3u
#define SHADE_LEVEL_DROP_TABLE   { 80u, 70u, 60u, 50u, 40u }  /* 档位 1..5 的掉点阈值 % */
#define SHADE_RELEASE_DIV        3u   /* 释放阈值 = 掉点阈值 / 3 (档位3 => 20%) */
#define SHADE_CONFIRM_N          3u   /* 连续 N 次(≈3x200ms)才翻转状态 */

/* =========================== CAN 2.0(经典 CAN / 500k / 标准帧) ==========
   接口 v2(2026-09-11): 语义事件接口 —— 总线上只传
   "事件码 + 语义参数 + 状态位 + 本节点 tick"，不传 lux/drop 百分比这类
   传感器数值；传感器怎么实现是下位机自己的事(见 docs/protocols.md §1)。 */
#define CAN_CMD_ID          0x100u   /* 主(M4)→从(C8T6) 指令帧 ID(0x1xx 段) */
#define CAN_ACK_ID          0x110u   /* 主(M4)→从 回执帧 ID(kind/code/arg) */
#define CAN_EVT_ID          0x200u   /* 从→主 事件帧 ID(语义事件) */
#define CAN_HB_ID           0x210u   /* 从→主 心跳帧 ID(1Hz; 查询应答同帧) */
#define CAN_POLL_PERIOD_MS     10u   /* CAN_Rx_Task 轮询周期 ms(收+心跳节拍) */
#define CAN_HB_PERIOD_MS     1000u   /* 心跳发送周期 ms */

/* 节点身份(0x210 d[5]/d[6]/d[7]): 主端据此识别"这一帧是谁发的"与固件版本 */
#define CAN_NODE_ID           0x01u  /* 节点号: 0x01 = C8T6 光感+舵机节点 */
#define CAN_DEV_TYPE          0x01u  /* 设备类型: 0x01 = 存在检测(光感) + SG90 道闸 */
#define CAN_FW_VER            0x20u  /* 固件版本: 高4位主版本/低4位次版本 => v2.0 */

/* =========================== SG90 道闸(设计功能已启用) ==================== */
/* TIM2_CH1/PA0, 72MHz, PSC=71/ARR=19999 -> 50Hz; CCR 值 = 脉宽 µs(@1MHz 计数)。
   0x100 指令 → gate.c 缓动到位 → 状态位 bit0/OLED 行3 反映执行到位(见 gate.h)。 */
#define SG90_TIM            htim2
#define SG90_CHANNEL        TIM_CHANNEL_1
#define GATE_CLOSE_CCR      500u     /* 0°   关闸 */
#define GATE_OPEN_CCR       1500u    /* 90°  开闸 */

/* =========================== OLED 显示(8x16 字体, 4 行 x 16 列) ========== */
#define OLED_LINE_TITLE        1u
#define OLED_LINE_LUX          2u
#define OLED_LINE_GATE         3u
#define OLED_LINE_CAN          4u
#define OLED_TITLE_STR        "PARK NODE"   /* 行1 正式标题(PhaseMd/02 P1-07 设计稿) */
#define OLED_REFRESH_MS      200u    /* OLED_Task 上屏周期 ms */
#define OLED_EVT_FLASH_MS   1000u    /* 发事件后 Line4 闪 "CAN:EVT" 时长 ms */
#define OLED_ACK_FLASH_MS   1000u    /* 收 0x110 回执后 Line4 显示 "CAN:Sended" 时长 ms */

#ifdef __cplusplus
}
#endif

#endif /* __CONFIG_H */
