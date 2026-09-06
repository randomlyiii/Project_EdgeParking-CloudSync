/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    bh1750.h
  * @brief   BH1750 环境光传感器驱动 (I2C1, 与 OLED 共用总线)
  *          ADDR 引脚接 GND -> 7位地址 0x23
  ******************************************************************************
  */
/* USER CODE END Header */
#ifndef __BH1750_H
#define __BH1750_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/* 传感器错误哨兵值：队列/上层用它区分"读不到"与"0 lx" */
#define BH1750_ERR_VALUE   0xFFFFu

/* 光照无效阈值(传感器裸露在强光下的量程上限附近)，供业务层参考 */
#define BH1750_LUX_MAX     54612u   /* H分辨率模式满量程 65535/1.2 */

HAL_StatusTypeDef BH1750_Init(void);
HAL_StatusTypeDef BH1750_ReadLux(uint16_t *lux);

#ifdef __cplusplus
}
#endif

#endif /* __BH1750_H */
