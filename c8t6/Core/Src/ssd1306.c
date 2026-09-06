/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    ssd1306.c
  * @brief   0.96" SSD1306 OLED 驱动实现 (软件 I2C, 8x16 字体, 4 行 x 16 列)
  *
  *          传输层为 sw_i2c(PB8/PB9 bit-bang)：F103 硬件 I2C 驱 SSD1306
  *          点不亮，Keil 参考工程软件模拟正常。
  *          显示 API 与参考工程 OLED 库同款(行 1~4 / 列 1~16, 8x16 字体)，
  *          内部 1024B 影子显存，Show* 只改显存，UpdateScreen 整屏上屏。
  ******************************************************************************
  */
/* USER CODE END Header */
#include "ssd1306.h"
#include "sw_i2c.h"
#include "oled_font8x16.h"
#include <string.h>

#define SSD1306_I2C_ADDR     0x78   /* 0x3C<<1，写地址字节，与参考工程一致 */

/* 控制字节: Co=0, D/C#=0 -> 后续全是命令; D/C#=1 -> 后续全是数据 */
#define SSD1306_CMD_BYTE     0x00u
#define SSD1306_DATA_BYTE    0x40u

/* 常用命令 */
#define SSD1306_DISPLAY_OFF          0xAEu
#define SSD1306_DISPLAY_ON           0xAFu
#define SSD1306_SET_PAGE_ADDR        0xB0u
#define SSD1306_SET_COL_ADDR_LOW     0x00u
#define SSD1306_SET_COL_ADDR_HIGH    0x10u

static uint8_t vram[SSD1306_WIDTH * SSD1306_HEIGHT / 8]; /* 1024B 显存 */

/* 发送命令流: START | 0x78 | 0x00 | cmd | STOP
 * 整个事务进临界区(短关中断)：bit-bang 期间若被 RTOS/中断抢占，可能落在
 * Stop/ACK 等危险窗口导致从机丢字节、页指针漂移(表现为整屏渐进乱码、
 * 碎像素掉到第 4 行等)。单条事务 < 100us，关中断无影响。 */
static HAL_StatusTypeDef SSD1306_WriteCmd(uint8_t cmd)
{
  __disable_irq();
  SW_I2C_Start();
  SW_I2C_SendByte(SSD1306_I2C_ADDR);
  SW_I2C_SendByte(SSD1306_CMD_BYTE);
  SW_I2C_SendByte(cmd);
  SW_I2C_Stop();
  __enable_irq();
  return HAL_OK;
}

/* 发送数据流: START | 0x78 | 0x40 | data[0..n-1] | STOP
 * 128 字节一次约 1.7ms(300kHz)，期间同样关中断保证不被打断；每页写完恢复，
 * 页与页之间仍可调度，不会饿死其它任务。 */
static void SSD1306_WriteData(const uint8_t *data, uint16_t len)
{
  __disable_irq();
  SW_I2C_Start();
  SW_I2C_SendByte(SSD1306_I2C_ADDR);
  SW_I2C_SendByte(SSD1306_DATA_BYTE);
  while (len--)
  {
    SW_I2C_SendByte(*data++);
  }
  SW_I2C_Stop();
  __enable_irq();
}

HAL_StatusTypeDef SSD1306_Init(void)
{
  /* 先把 PB8/PB9 reclaim 成 GPIO 开漏(覆盖 MspInit 里的 I2C1 复用配置)，
     之后 hi2c1 不再可用，BH1750 也走本软件总线 */
  SW_I2C_Init();

  /* 上电稳定等待(与参考工程同款忙等，72MHz 下约 20-30ms) */
  for (volatile uint32_t i = 0; i < 1000000u; i++)
  {
    __NOP();
  }

  /* ⚠️ 命令字节与参数字节必须逐字节独立发送(与参考工程 OLED_Config 一致)。
     历史教训：旧版只发命令不发参数，0x8D(电荷泵)把后一条命令当参数吃掉，
     内部升压被关 => 面板无 VPP 全黑，而 I2C 地址应答一切正常(极难排查) */
  static const uint8_t initSeq[] = {
    0xAE,        /* 关显示(防止改配置期间花屏) */
    0xD5, 0x80,  /* 时钟分频/振荡器 */
    0xA8, 0x3F,  /* 多路复用 64 行 */
    0xD3, 0x00,  /* 显示偏移 0 */
    0x40,        /* 起始行 0 */
    0xA1,        /* 段重映射(左右方向) */
    0xC8,        /* COM 扫描方向(上下方向) */
    0xDA, 0x12,  /* COM 引脚配置 */
    0x81, 0xCF,  /* 对比度 */
    0xD9, 0xF1,  /* 预充电周期 */
    0xDB, 0x30,  /* VCOMH 电平 */
    0x20, 0x02,  /* 页寻址模式 */
    0xA4,        /* 显示跟随显存 */
    0xA6,        /* 正常显示(非反显) */
    0x8D, 0x14,  /* ⭐ 电荷泵开启(内部升压，面板供电来源) */
  };
  for (uint32_t i = 0; i < sizeof(initSeq); i++)
  {
    SSD1306_WriteCmd(initSeq[i]);
  }

  /* 先清屏重绘再开显示(顺序与参考工程一致，避免上电花屏) */
  SSD1306_Fill(0x00);
  SSD1306_UpdateScreen();
  return SSD1306_WriteCmd(SSD1306_DISPLAY_ON);
}

void SSD1306_Fill(uint8_t pattern)
{
  memset(vram, pattern, sizeof(vram));
}

HAL_StatusTypeDef SSD1306_UpdateScreen(void)
{
  /* 逐页写: 设页地址+列地址(命令)，再跟 128 字节数据，与参考工程一致 */
  for (uint8_t page = 0; page < 8; page++)
  {
    SSD1306_WriteCmd(SSD1306_SET_PAGE_ADDR | page);
    SSD1306_WriteCmd(SSD1306_SET_COL_ADDR_LOW);  /* 列低 4 位 = 0 */
    SSD1306_WriteCmd(SSD1306_SET_COL_ADDR_HIGH); /* 列高 4 位 = 0 */
    SSD1306_WriteData(&vram[page * SSD1306_WIDTH], SSD1306_WIDTH);
  }
  return HAL_OK;
}

/* 在 (line, column) 画一个 8x16 字符(只写显存)。
   line 1~4(每行 16 像素 = 2 页)，column 1~16；越界忽略 */
void SSD1306_ShowChar(uint8_t line, uint8_t column, char ch)
{
  if (line < 1u || line > SSD1306_LINES ||
      column < 1u || column > SSD1306_COLUMNS)
  {
    return;
  }
  if (ch < 0x20 || ch > 0x7E)
  {
    ch = ' ';
  }
  uint8_t idx  = (uint8_t)(ch - 0x20);
  uint8_t page = (uint8_t)((line - 1u) * 2u);
  uint8_t x    = (uint8_t)((column - 1u) * SSD1306_FONT_W);

  for (uint8_t i = 0; i < 8; i++)
  {
    vram[page * SSD1306_WIDTH + x + i] = font8x16[idx][i];       /* 上半 */
    vram[(page + 1u) * SSD1306_WIDTH + x + i] = font8x16[idx][i + 8]; /* 下半 */
  }
}

/* 从 (line, column) 起显示字符串(只写显存)，超出屏幕右端的字符忽略 */
void SSD1306_ShowString(uint8_t line, uint8_t column, const char *str)
{
  if (str == NULL)
  {
    return;
  }
  while (*str)
  {
    SSD1306_ShowChar(line, column, *str++);
    column++;
  }
}
