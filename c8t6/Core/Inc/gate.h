/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    gate.h
  * @brief   SG90 道闸执行层: PWM 输出 + 缓动到位 + 到位状态(设计功能 P1-06/P2-02)
  *
  *          与 C8T6 上层约定:
  *          - can_node.c: 收到 0x100/0x01 开闸 → Gate_SetTarget(1);
  *            0x100/0x02 关闸 → Gate_SetTarget(0);
  *          - CAN_Rx_Task(10ms 节拍) 经 CAN_Node_Poll 调 Gate_Poll() 缓动;
  *          - 状态位 bit0"闸开" 与 OLED 行3 一律读 Gate_IsOpen/IsMoving(执行到位,
  *            非指令态) —— 舵机未到位前保持关/动, 状态真实。
  *          ⚠️ SG90 必须独立 5~6V 供电并共地(接 MCU 5V 会复位/发烫, PhaseMd/02 避坑)。
  ******************************************************************************
  */
/* USER CODE END Header */
#ifndef __GATE_H__
#define __GATE_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 上电自检: 1=Gate_Init 时执行 0°→90°→0°(验接线/供电); 稳定后置 0 */
#define GATE_SELFTEST      1u

/* Gate_Poll 单次位移(µs)。10ms 节拍 ×50µs → 满行程 1000µs ≈ 0.2s,
   缓动避免 CCR 突变引舵机抖动啸叫 */
#define GATE_STEP_US      50u

void    Gate_Init(void);            /* PWM Start + 置关位 + 可选自检(调度器启动前调用) */
void    Gate_SetTarget(uint8_t open);   /* 1=开到 90°(GATE_OPEN_CCR)  0=回 0°(GATE_CLOSE_CCR) */
void    Gate_Poll(void);            /* 缓动步进, 每 10ms 调一次(未到位才动作) */
uint8_t Gate_IsOpen(void);          /* 1=已执行到位(开) */
uint8_t Gate_IsMoving(void);        /* 1=正在缓动途中 */

#ifdef __cplusplus
}
#endif

#endif /* __GATE_H__ */
