/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    shade.h
  * @brief   遮光检测状态机(百分比掉点 + 回滞去抖) — 供 BH1750_Task 与 CAN 上报使用
  *
  *          语义(与 PhaseMd/02 P1-05 / c8t6/实现步骤.md 阶段2 一致)：
  *          - 环境光基线 EMA 慢跟踪，变亮快速跟上；遮光中基线冻结防塌陷；
  *          - 掉点 drop% >= SHADE_DROP_PERCENT 连续 SHADE_CONFIRM_N 次 => 进入遮光；
  *            掉点 < SHADE_RELEASE_PERCENT 连续 N 次 => 恢复(回滞)；
  *          - 只在状态翻转边沿返回 ±1，业务层(0x200 事件)据此发帧。
  *          阈值常量在 config.h，现场只调那里。
  ******************************************************************************
  */
/* USER CODE END Header */
#ifndef __SHADE_H
#define __SHADE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

typedef enum {
  SHADE_CLEAR   = 0,   /* 未遮光 */
  SHADE_SHADED  = 1    /* 遮光中(视为"车到位") */
} ShadeState_t;

/* 喂入一次有效 lux(每 BH1750 采样周期一次)。
   返回: 1=刚进入遮光(边沿)  -1=刚恢复(边沿)  0=状态无变化。
   传感器读取失败的周期不要喂(改调 Shade_ReportFault)。 */
int8_t  Shade_FSM_Update(uint16_t lux);

/* 传感器本周期是否故障(1=读不到/读取失败)；由 BH1750_Task 每次采样后上报，
   用于状态位 bit2"光感故障"与 OLED 显示。 */
void    Shade_ReportFault(uint8_t fault);

uint8_t  Shade_IsShaded(void);          /* 0=CLEAR 1=SHADED(当前状态) */
uint8_t  Shade_GetDrop(void);           /* 当前 drop%(0~100)，供 OLED/0x200 */
uint16_t Shade_GetLuxLast(void);        /* 最近一次有效 lux 快照(查询应答用) */
uint8_t  Shade_SensorFault(void);       /* 最近一次采样是否故障 */

#ifdef __cplusplus
}
#endif

#endif /* __SHADE_H */
