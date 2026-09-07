/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    fdcan.h
  * @brief   This file contains all the function prototypes for
  *          the fdcan.c file
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __FDCAN_H__
#define __FDCAN_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

extern FDCAN_HandleTypeDef hfdcan2;

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

void MX_FDCAN2_Init(void);

/* USER CODE BEGIN Prototypes */

/* 运行期 FDCAN 时钟实测与 500k 位时序换算(实现见 fdcan.c 尾部)。
   main.c: MX_FDCAN2_Init() 之后、CAN_Master_Init()(Start) 之前调用一次。 */
void FDCAN2_AutotuneBitTiming(void);

/* 调试器 live watch: 实测时钟与换算结果(工程模式=100MHz / Linux引导=62.5MHz) */
extern volatile uint32_t g_fdcan_meas_hz;
extern volatile uint32_t g_fdcan_cfg_pre;
extern volatile uint32_t g_fdcan_cfg_seg1;
extern volatile uint32_t g_fdcan_cfg_seg2;

/* USER CODE END Prototypes */

#ifdef __cplusplus
}
#endif

#endif /* __FDCAN_H__ */

