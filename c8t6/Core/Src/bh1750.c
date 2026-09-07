/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    bh1750.c
  * @brief   BH1750 环境光传感器驱动实现(软件 I2C, 与 OLED 共用总线)
  *
  *          总线约定(遵循 MD文档/oled_standard.md):
  *          - PB8=SCL / PB9=SDA, GPIO 开漏, 引脚由 SW_I2C_Init 配置一次,
  *            本驱动不重复配置 GPIO(与参考工程同款约定);
  *          - 与 SSD1306(0x3C) 同一条总线, 本器件 7 位地址 0x23(ADDR=GND);
  *          - 多任务共享总线: 调用方必须持 OledMutex(freertos.c) 把本驱动
  *            的调用串行化, 否则两个任务的 bit-bang 事务会互相插坏;
  *          - EMI 提示: CAN RX(PA11) 高速翻转可能耦合干扰本总线, CAN 阶段
  *            联调若见 I2C 读数异常, 参考 oled_standard.md 6.1 做物理隔离。
  *
  *          时序: 连续 H 分辨率模式(0x10), 测量周期约 120ms, lx = raw/1.2。
  *          初始化采用已验证参考序列(参考资料/bh1750.c):
  *          POWER_ON -> 10ms -> RESET -> 10ms -> CONT_HRES -> 180ms
  *          (等首次测量完成), 全程阻塞约 200ms。
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
#define BH1750_CMD_CONT_HRES 0x10u         /* 连续 H 分辨率，120ms 典型 */

/* 初始化各步间隔(ms)，取自已验证参考工程 */
#define BH1750_DELAY_CMD_MS   10u
#define BH1750_DELAY_FIRST_MS 180u

/* 发一条单字节命令，地址字节做应答检测：NACK => 器件不在总线 */
static HAL_StatusTypeDef BH1750_WriteCmd(uint8_t cmd)
{
  HAL_StatusTypeDef st = HAL_OK;

  SW_I2C_Start();
  if (SW_I2C_SendByteAck(BH1750_I2C_ADDR_W) != 0u)   /* 0=ACK, 1=NACK */
  {
    st = HAL_ERROR;
  }
  else
  {
    SW_I2C_SendByte(cmd);
  }
  SW_I2C_Stop();      /* NACK 时也要发 Stop，恢复总线空闲态 */
  return st;
}

HAL_StatusTypeDef BH1750_Init(void)
{
  /* POWER_ON -> RESET -> 连续 H 分辨率，命令间留间隔(参考工程实测序列) */
  if (BH1750_WriteCmd(BH1750_CMD_POWER_ON) != HAL_OK)  { return HAL_ERROR; }
  HAL_Delay(BH1750_DELAY_CMD_MS);
  if (BH1750_WriteCmd(BH1750_CMD_RESET) != HAL_OK)     { return HAL_ERROR; }
  HAL_Delay(BH1750_DELAY_CMD_MS);
  if (BH1750_WriteCmd(BH1750_CMD_CONT_HRES) != HAL_OK) { return HAL_ERROR; }

  /* 进连续模式后传感器自动循环测量，等首次转换完成再返回，防读到半截结果 */
  HAL_Delay(BH1750_DELAY_FIRST_MS);
  return HAL_OK;
}

HAL_StatusTypeDef BH1750_ReadLux(uint16_t *lux)
{
  uint8_t hi, lo;
  uint32_t raw;

  if (lux == NULL)
  {
    return HAL_ERROR;
  }

  /* BH1750 无寄存器地址: 直接发读地址，连续收 2 字节(高在前)。
     地址字节做应答检测：传感器掉线/未上电时报错而非读回垃圾，
     上层凭 HAL_ERROR 置 BH1750_ERR_VALUE */
  SW_I2C_Start();
  if (SW_I2C_SendByteAck(BH1750_I2C_ADDR_R) != 0u)
  {
    SW_I2C_Stop();
    return HAL_ERROR;
  }
  hi = SW_I2C_RecvByte(1);   /* 首字节回 ACK */
  lo = SW_I2C_RecvByte(0);   /* 末字节回 NACK */
  SW_I2C_Stop();

  raw = ((uint32_t)hi << 8) | lo;
  /* H 分辨率模式换算: lx = raw / 1.2，+0.6 做四舍五入 */
  *lux = (uint16_t)((raw * 10u + 6u) / 12u);
  return HAL_OK;
}
