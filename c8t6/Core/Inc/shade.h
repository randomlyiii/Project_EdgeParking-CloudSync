/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    shade.h
  * @brief   存在检测状态机(百分比掉点 + 回滞去抖) — 供 BH1750_Task 与 CAN 上报使用
  *
  *          语义(与 PhaseMd/02 P1-05 / c8t6/实现步骤.md 阶段2 一致)：
  *          - 环境光基线 EMA 慢跟踪，变亮快速跟上；遮光中基线冻结防塌陷；
  *          - 掉点 >= 当前档位阈值 连续 SHADE_CONFIRM_N 次 => "有车/物体"(PRESENCE)；
  *            掉点 < 释放阈值 连续 N 次 => 离开(回滞)；
  *          - 只在状态翻转边沿返回 ±1，业务层(0x200 语义事件)据此发帧。
  *          阈值常量/档位表在 config.h，现场只调那里；档位由板级经
  *          CAN 0x100/0x10(SET_DETECT_LEVEL)下发，见 Shade_SetDetectLevel()。
  *
  *          ⭐ 本模块产出的 lux/drop% 只供**本节点 OLED 显示**，不上 CAN 总线
  *          (板子层只收"车到位/车离开"语义事件，见 can_node.h)。
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
  SHADE_CLEAR   = 0,   /* 无车/未检出 */
  SHADE_SHADED  = 1    /* 检出(视为"车到位") */
} ShadeState_t;

/* 喂入一次有效 lux(每 BH1750 采样周期一次)。
   返回: 1=刚进入检出(边沿)  -1=刚离开(边沿)  0=状态无变化。
   传感器读取失败的周期不要喂(改调 Shade_ReportFault)。 */
int8_t  Shade_FSM_Update(uint16_t lux);

/* 传感器本周期是否故障(1=读不到/读取失败)；由 BH1750_Task 每次采样后上报，
   用于状态位 bit2"传感器故障"与 OLED 显示。 */
void    Shade_ReportFault(uint8_t fault);

uint8_t  Shade_IsShaded(void);          /* 0=CLEAR 1=检出(当前状态) */
uint8_t  Shade_GetDrop(void);           /* 当前 drop%(0~100)，仅本节点 OLED 用 */
uint16_t Shade_GetLuxLast(void);        /* 最近一次有效 lux 快照，仅本节点 OLED/调试用 */
uint8_t  Shade_SensorFault(void);       /* 最近一次采样是否故障 */

/* ---------- 灵敏度档位(CAN 0x100/0x10 语义参数) ---------- */
/* 设置档位(1..5)：合法则立即生效并返回 0；越界返回 1(保持原档位)。
   档位 -> 掉点阈值 的映射表在 config.h(SHADE_LEVEL_DROP_TABLE)。 */
uint8_t  Shade_SetDetectLevel(uint8_t level);
uint8_t  Shade_GetDetectLevel(void);    /* 当前档位 1..5(上电为 SHADE_LEVEL_DEFAULT) */
uint8_t  Shade_GetDropThreshold(void);  /* 当前档位对应的掉点阈值 % */

#ifdef __cplusplus
}
#endif

#endif /* __SHADE_H */
