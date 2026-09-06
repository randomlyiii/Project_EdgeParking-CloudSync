/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    sw_i2c.h
  * @brief   软件 I2C 总线 (PB8=SCL / PB9=SDA, GPIO 开漏模拟)
  *
  *          背景：F103 硬件 I2C1 + HAL 驱 SSD1306 点不亮，而 Keil 工程
  *          (Project_c8t6+oled_Check) 用同引脚软件模拟 I2C 显示正常，
  *          故整体切换为 bit-bang 方案；时序与该参考工程一致。
  *          OLED(0x3C) 与 BH1750(0x23) 挂在同一条软件总线上。
  *          注意：SSD1306_Init 会把 PB8/PB9 从硬件 I2C1 复用 reclaim 回
  *          普通 GPIO，此后 hi2c1 不再可用。
  ******************************************************************************
  */
/* USER CODE END Header */
#ifndef __SW_I2C_H
#define __SW_I2C_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/* 引脚定义(与硬件 I2C1 重映射位置一致，接线无需改动) */
#define SW_I2C_SCL_Pin        GPIO_PIN_8
#define SW_I2C_SDA_Pin        GPIO_PIN_9
#define SW_I2C_GPIO_Port      GPIOB

void     SW_I2C_Init(void);
void     SW_I2C_Start(void);
void     SW_I2C_Stop(void);
/* 发送 1 字节(忽略应答，与参考工程一致) */
void     SW_I2C_SendByte(uint8_t byte);
/* 发送 1 字节并采样第 9 位：返回 0=从机 ACK，1=NACK */
uint8_t  SW_I2C_SendByteAck(uint8_t byte);
/* 接收 1 字节；ack=1 主机回 ACK，ack=0 回 NACK(最后一个字节) */
uint8_t  SW_I2C_RecvByte(uint8_t ack);
/* 探测写地址 addrW 是否有从机应答：返回 0=ACK，1=无应答 */
uint8_t  SW_I2C_Probe(uint8_t addrW);
/* 总线自检：返回 0=正常；1=SCL拉不低 2=SCL松不开 3=SDA拉不低 4=SDA松不开 */
uint8_t  SW_I2C_LineCheck(void);

#ifdef __cplusplus
}
#endif

#endif /* __SW_I2C_H */
