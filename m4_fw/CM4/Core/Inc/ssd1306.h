/**
 ******************************************************************************
 * @file    ssd1306.h
 * @brief   0.96" OLED (SSD1306, 128x64, I2C) driver for STM32MP1 M4 (HAL)
 *          使用：本文件放 Core/Inc，ssd1306.c 放 Core/Src
 ******************************************************************************
 */
#ifndef __SSD1306_H
#define __SSD1306_H

#include <stdint.h>

/* 屏幕尺寸 */
#define SSD1306_WIDTH           128
#define SSD1306_HEIGHT          64
#define SSD1306_PAGES           (SSD1306_HEIGHT / 8)   /* 8 */
#define SSD1306_BUFFER_SIZE     (SSD1306_WIDTH * SSD1306_PAGES)  /* 1024 */

/* I2C 从机地址：模块常见 0x3C（SA0 接地）或 0x3D（SA0 接高）。
 * HAL 的地址参数是 8bit 写地址 = 7bit 地址 << 1，故 0x3C << 1 = 0x78 */
#define SSD1306_I2C_ADDR        (0x3C << 1)

/* ---- 初始化：上电后调用一次，之后任意函数操作显示缓存 ---- */
void ssd1306_Init(void);

/* ---- 清屏：0=全灭，1=全亮 ---- */
void ssd1306_Clear(uint8_t fill);

/* ---- 把显示缓存刷到屏幕（画字后必须调用才可见） ---- */
void ssd1306_Update(void);

/* ---- 设置光标：x=0..127(列), page=0..7(页, 每页8像素高) ---- */
void ssd1306_SetCursor(uint8_t x, uint8_t page);

/* ---- 写字符串（6x8 ASCII；小写自动转大写显示），'\n' 换页 ---- */
void ssd1306_WriteString(const char *str);

#endif /* __SSD1306_H */
