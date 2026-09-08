/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    tim.h
  * @brief   TIM2 PWM Generation CH1 (PA0) — SG90 道闸 50Hz
  *
  *          手写的 CubeMX 等价实现: 72MHz / PSC=71 → 1MHz 计数(1µs/计数) /
  *          ARR=19999 → 50Hz; CCR 值 = 脉宽 µs
  *          (GATE_OPEN_CCR=1500µs/90°, GATE_CLOSE_CCR=500µs/0°, 见 config.h)。
  *          ⚠️ regen 注意: 若再用 CubeMX 重新生成, 需先在 .ioc 勾选
  *          TIM2 → PWM Generation CH1(PA0), 否则本文件与 main.c 调用会被覆盖删除。
  ******************************************************************************
  */
/* USER CODE END Header */
#ifndef __TIM_H__
#define __TIM_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

extern TIM_HandleTypeDef htim2;

void MX_TIM2_Init(void);

#ifdef __cplusplus
}
#endif

#endif /* __TIM_H__ */
