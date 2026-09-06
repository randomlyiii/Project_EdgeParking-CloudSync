/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    ssd1306.h
  * @brief   0.96" SSD1306 OLED 驱动 (软件 I2C, 8x16 字体, 4 行 x 16 列)
  *
  *          显示 API 与参考工程(Keil OLED 库)同款：行 1~4、列 1~16，
  *          每字符 8x16 像素(占 2 个页)。内部 1024B 影子显存，
  *          Show* 只改显存，调 SSD1306_UpdateScreen() 才上屏。
  *          与 BH1750 共用 sw_i2c 软件总线；显示互斥由应用层 OledMutex 保证。
  ******************************************************************************
  */
/* USER CODE END Header */
#ifndef __SSD1306_H
#define __SSD1306_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

#define SSD1306_WIDTH      128u
#define SSD1306_HEIGHT     64u

/* 8x16 字体: 宽 8 高 16，每字符占 2 个页；屏共 4 行 x 16 列 */
#define SSD1306_FONT_W          8u
#define SSD1306_FONT_H          16u
#define SSD1306_LINES           4u
#define SSD1306_COLUMNS         16u

HAL_StatusTypeDef SSD1306_Init(void);
void SSD1306_Fill(uint8_t pattern);
HAL_StatusTypeDef SSD1306_UpdateScreen(void);
void SSD1306_ShowChar(uint8_t line, uint8_t column, char ch);
void SSD1306_ShowString(uint8_t line, uint8_t column, const char *str);

#ifdef __cplusplus
}
#endif

#endif /* __SSD1306_H */
