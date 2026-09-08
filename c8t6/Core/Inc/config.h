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

/* =========================== 遮光检测(百分比掉点状态机, shade.c) ========== */
/* BH1750 采样周期以 bh1750.h 的 BH1750_READ_PERIOD_MS(=200ms) 为准，
   状态机每周期喂一次有效 lux；去抖 = 连续 SHADE_CONFIRM_N 次满足才翻转。 */
#define SHADE_DROP_PERCENT     60u   /* lux 相对基线掉 >=60% 判"遮光/车到位" */
#define SHADE_RELEASE_PERCENT  20u   /* 掉点 <20% 判"恢复"(回滞, 防临界抖动) */
#define SHADE_CONFIRM_N         3u   /* 连续 N 次(≈3x200ms)才翻转状态 */

/* =========================== CAN 2.0(经典 CAN / 500k / 标准帧) ========== */
#define CAN_CMD_ID          0x100u   /* 主(M4)→从(C8T6) 指令帧 ID(0x1xx 段) */
#define CAN_ACK_ID          0x110u   /* 主(M4)→从 事件确认帧 ID: 收到 0x200 后回执,
                                        d[0]=回显事件码(0x01 遮光/0x00 恢复) */
#define CAN_EVT_ID          0x200u   /* 从→主 事件帧 ID */
#define CAN_HB_ID           0x210u   /* 从→主 心跳帧 ID(1Hz) */
#define CAN_POLL_PERIOD_MS     10u   /* CAN_Rx_Task 轮询周期 ms(收+心跳节拍) */
#define CAN_HB_PERIOD_MS     1000u   /* 心跳发送周期 ms */

/* =========================== SG90 道闸(未挂载前占位) ===================== */
/* 硬件(PWM)后加：TIM2_CH1/PA0, 72MHz, PSC=71/ARR=19999 -> 50Hz；
   CCR 值 = 脉宽 µs(@1MHz 计数)。当前开/关闸仅更新逻辑状态 + OLED。
   NOTE: SG90_TIM 仅宏占位，SG90 阶段实现 Gate_StepTo 后使用。 */
#define SG90_TIM            htim2
#define SG90_CHANNEL        TIM_CHANNEL_1
#define GATE_CLOSE_CCR      500u     /* 0°   关闸 */
#define GATE_OPEN_CCR       1500u    /* 90°  开闸 */

/* =========================== OLED 显示(8x16 字体, 4 行 x 16 列) ========== */
#define OLED_LINE_TITLE        1u
#define OLED_LINE_LUX          2u
#define OLED_LINE_GATE         3u
#define OLED_LINE_CAN          4u
#define OLED_REFRESH_MS      200u    /* OLED_Task 上屏周期 ms */
#define OLED_EVT_FLASH_MS   1000u    /* 遮光事件后 Line4 闪 "CAN:EVT" 时长 ms */
#define OLED_ACK_FLASH_MS   1000u    /* 收 0x110 确认后 Line4 显示 "CAN:*OK*" 时长 ms */

#ifdef __cplusplus
}
#endif

#endif /* __CONFIG_H */
