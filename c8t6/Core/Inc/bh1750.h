/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    bh1750.h
  * @brief   BH1750 环境光传感器驱动 (软件 I2C, 与 OLED 共用 PB8/PB9 总线)
  *          ADDR 引脚接 GND -> 7位地址 0x23
  *
  *          调用约定(oled_standard.md / 共享总线):
  *          - 引脚由 SW_I2C_Init 配置一次, 本驱动不改 GPIO;
  *          - 调用方须持 OledMutex(freertos.c) 串行化总线访问;
  *          - BH1750_Init 阻塞约 200ms(含等首次测量), 仅初始化/重初始化时调。
  ******************************************************************************
  */
/* USER CODE END Header */
#ifndef __BH1750_H
#define __BH1750_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/* 读取/采样周期(ms)：连续 H 模式测量周期约 120~180ms，200ms 匹配 */
#define BH1750_READ_PERIOD_MS  200u

/* 连续失败多少个采样周期后重发初始化序列(传感器后插上/掉电自愈) */
#define BH1750_REINIT_FAILS    25u   /* 25 x 200ms = 5s */

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
