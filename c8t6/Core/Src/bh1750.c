/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    bh1750.c
  * @brief   BH1750 环境光传感器驱动实现
  *
  *          连续 H 分辨率模式(0x10)：测量周期约 120ms，lx = raw / 1.2。
  *          本任务以 200ms 周期读取，与测量周期匹配。
  ******************************************************************************
  */
/* USER CODE END Header */
#include "bh1750.h"
#include "sw_i2c.h"

/* 与 OLED 共用 sw_i2c 软件总线(PB8/PB9)。ADDR=GND 时 7 位地址 0x23 */
#define BH1750_I2C_ADDR_W    0x46   /* 0x23<<1 | W */
#define BH1750_I2C_ADDR_R    0x47   /* 0x23<<1 | R */

/* 指令集 */
#define BH1750_CMD_POWER_ON  0x01u
#define BH1750_CMD_RESET     0x07u
#define BH1750_CMD_CONT_HRES 0x10u         /* 连续 H 分辨率，180ms 典型 */

static HAL_StatusTypeDef BH1750_WriteCmd(uint8_t cmd)
{
  SW_I2C_Start();
  SW_I2C_SendByte(BH1750_I2C_ADDR_W);
  SW_I2C_SendByte(cmd);
  SW_I2C_Stop();
  return HAL_OK;
}

HAL_StatusTypeDef BH1750_Init(void)
{
  HAL_StatusTypeDef st;

  st = BH1750_WriteCmd(BH1750_CMD_POWER_ON);
  if (st != HAL_OK)
  {
    return st;
  }
  st = BH1750_WriteCmd(BH1750_CMD_RESET);
  if (st != HAL_OK)
  {
    return st;
  }
  /* 进入连续 H 分辨率模式后传感器自动循环测量，之后只需读数 */
  return BH1750_WriteCmd(BH1750_CMD_CONT_HRES);
}

HAL_StatusTypeDef BH1750_ReadLux(uint16_t *lux)
{
  uint8_t hi, lo;
  uint32_t raw;

  if (lux == NULL)
  {
    return HAL_ERROR;
  }

  /* BH1750 无寄存器地址: 直接发读地址，连续收 2 字节(高在前) */
  SW_I2C_Start();
  SW_I2C_SendByte(BH1750_I2C_ADDR_R);
  hi = SW_I2C_RecvByte(1);   /* 首字节回 ACK */
  lo = SW_I2C_RecvByte(0);   /* 末字节回 NACK */
  SW_I2C_Stop();

  raw = ((uint32_t)hi << 8) | lo;
  /* H 分辨率模式换算: lx = raw / 1.2，+0.6 做四舍五入 */
  *lux = (uint16_t)((raw * 10u + 6u) / 12u);
  return HAL_OK;
}
