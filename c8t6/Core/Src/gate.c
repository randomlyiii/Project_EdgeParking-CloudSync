/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    gate.c
  * @brief   SG90 道闸执行层实现(设计功能 P1-06/P2-02, 见 gate.h 头注)
  ******************************************************************************
  */
/* USER CODE END Header */

#include "gate.h"
#include "config.h"     /* SG90_TIM/SG90_CHANNEL/GATE_OPEN_CCR/GATE_CLOSE_CCR */
#include "tim.h"
#include "main.h"

/* 私有状态: 单写者 —— s_cur/s_tgt 只在 main 裸机段(Gate_Init)与 CAN_Rx_Task
   (CAN_Node_Poll → Gate_Poll) 修改; OLED/状态位只读, 16 位读写原子足够 */
static volatile uint16_t s_cur_ccr = GATE_CLOSE_CCR;
static volatile uint16_t s_tgt_ccr = GATE_CLOSE_CCR;
static volatile uint8_t  s_moving  = 0u;

/* 向目标步进一次(GATE_STEP_US), 到位自动停; 返回 1=仍在移动 */
static uint8_t Gate_StepOnce(void)
{
  uint16_t cur = s_cur_ccr;

  if (cur < s_tgt_ccr)
  {
    cur += GATE_STEP_US;
    if (cur >= s_tgt_ccr) { cur = s_tgt_ccr; }
  }
  else if (cur > s_tgt_ccr)
  {
    cur -= GATE_STEP_US;
    if (cur <= s_tgt_ccr) { cur = s_tgt_ccr; }
  }

  s_cur_ccr = cur;
  __HAL_TIM_SET_COMPARE(&SG90_TIM, SG90_CHANNEL, cur);
  s_moving = (cur != s_tgt_ccr) ? 1u : 0u;
  return s_moving;
}

void Gate_Init(void)
{
  __HAL_TIM_SET_COMPARE(&SG90_TIM, SG90_CHANNEL, GATE_CLOSE_CCR);
  if (HAL_TIM_PWM_Start(&SG90_TIM, SG90_CHANNEL) != HAL_OK)
  {
    Error_Handler();
  }
  s_cur_ccr = GATE_CLOSE_CCR;
  s_tgt_ccr = GATE_CLOSE_CCR;
  s_moving  = 0u;

#if (GATE_SELFTEST == 1u)
  /* 上电自检: 0°→90° 保持 0.5s → 回 0°(每步约 10ms, 单程 ≤0.2s; 防死循环加迭代上限) */
  Gate_SetTarget(1u);
  for (uint8_t i = 0u; i < 250u; i++)
  {
    (void)Gate_StepOnce();
    HAL_Delay(10u);
    if (!Gate_IsMoving()) { break; }
  }
  HAL_Delay(500u);
  Gate_SetTarget(0u);
  for (uint8_t i = 0u; i < 250u; i++)
  {
    (void)Gate_StepOnce();
    HAL_Delay(10u);
    if (!Gate_IsMoving()) { break; }
  }
#endif
}

void Gate_SetTarget(uint8_t open)
{
  uint16_t t = open ? GATE_OPEN_CCR : GATE_CLOSE_CCR;
  if (t != s_tgt_ccr)
  {
    s_tgt_ccr = t;
    s_moving  = 1u;
  }
}

void Gate_Poll(void)
{
  if (s_moving != 0u)
  {
    (void)Gate_StepOnce();
  }
}

uint8_t Gate_IsOpen(void)
{
  return (s_cur_ccr >= GATE_OPEN_CCR) ? 1u : 0u;
}

uint8_t Gate_IsMoving(void)
{
  return s_moving;
}
