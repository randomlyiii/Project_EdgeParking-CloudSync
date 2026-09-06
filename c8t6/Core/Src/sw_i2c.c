/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    sw_i2c.c
  * @brief   软件 I2C 总线实现 (PB8=SCL / PB9=SDA)
  *
  *          时序复刻自已验证可用的 Keil 参考工程：无显式延时，
  *          GPIO 开漏 + 模块板载上拉，速率约 100~300kHz。
  *          引脚在 Init 时配置为 GPIO 开漏输出——若此前 MX_I2C1_Init
  *          已把 PB8/PB9 配成 I2C1 复用，这里会一并覆盖回 GPIO 控制。
  ******************************************************************************
  */
/* USER CODE END Header */
#include "sw_i2c.h"

#define SCL(x)  HAL_GPIO_WritePin(SW_I2C_GPIO_Port, SW_I2C_SCL_Pin, \
                                  (x) ? GPIO_PIN_SET : GPIO_PIN_RESET)
#define SDA(x)  HAL_GPIO_WritePin(SW_I2C_GPIO_Port, SW_I2C_SDA_Pin, \
                                  (x) ? GPIO_PIN_SET : GPIO_PIN_RESET)
#define SCL_READ()  HAL_GPIO_ReadPin(SW_I2C_GPIO_Port, SW_I2C_SCL_Pin)
#define SDA_READ()  HAL_GPIO_ReadPin(SW_I2C_GPIO_Port, SW_I2C_SDA_Pin)

/* 每个边沿约 1~2us 的空转延时：72MHz 下裸 HAL 翻转约 1MHz+，
   杜邦线+4.7k 上拉的 RC 上升沿可能跟不上；压到 ~200-300kHz 保稳 */
static void SW_I2C_Delay(void)
{
  for (volatile uint32_t i = 0; i < 40; i++)
  {
    __NOP();
  }
}

void SW_I2C_Init(void)
{
  GPIO_InitTypeDef g = {0};

  __HAL_RCC_GPIOB_CLK_ENABLE();

  /* 开漏输出：写 ODR 控制拉低，释放时靠模块上拉回高；
     开漏下读 IDR 可取到 SDA 实际电平(用于读字节/应答位) */
  g.Pin   = SW_I2C_SCL_Pin | SW_I2C_SDA_Pin;
  g.Mode  = GPIO_MODE_OUTPUT_OD;
  g.Pull  = GPIO_NOPULL;
  g.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(SW_I2C_GPIO_Port, &g);

  /* 总线空闲态：SCL/SDA 均为高 */
  SCL(1);
  SDA(1);
}

void SW_I2C_Start(void)
{
  SDA(1);
  SCL(1);
  SW_I2C_Delay();
  SDA(0);
  SW_I2C_Delay();
  SCL(0);
  SW_I2C_Delay();
}

void SW_I2C_Stop(void)
{
  SDA(0);
  SW_I2C_Delay();
  SCL(1);
  SW_I2C_Delay();
  SDA(1);
}

void SW_I2C_SendByte(uint8_t byte)
{
  (void)SW_I2C_SendByteAck(byte);
}

/* 发送 1 字节并采样第 9 位应答：返回 0=从机 ACK，1=NACK/无人应答。
   开漏模式下读 IDR 即引脚真实电平 */
uint8_t SW_I2C_SendByteAck(uint8_t byte)
{
  /* 高位在前 */
  for (uint8_t i = 0; i < 8; i++)
  {
    SDA((byte & (0x80 >> i)) != 0);
    SW_I2C_Delay();
    SCL(1);
    SW_I2C_Delay();
    SCL(0);
    SW_I2C_Delay();
  }
  SDA(1);              /* 释放 SDA，由从机在第 9 个时钟拉低应答 */
  SW_I2C_Delay();
  SCL(1);
  SW_I2C_Delay();
  uint8_t ack = SDA_READ() ? 1u : 0u;
  SCL(0);
  SW_I2C_Delay();
  return ack;
}

/* 探测写地址是否有从机应答：返回 0=ACK(器件在总线)，1=无应答 */
uint8_t SW_I2C_Probe(uint8_t addrW)
{
  uint8_t ack;

  SW_I2C_Start();
  ack = SW_I2C_SendByteAck(addrW);
  SW_I2C_Stop();
  return ack;
}

/* 总线自检(细分到每一步)：依次测 SCL 拉低/SCL 释放/SDA 拉低/SDA 释放。
   返回 0=全部正常；1=SCL拉不低 2=SCL松不开 3=SDA拉不低 4=SDA松不开
   (开漏下读 IDR 即引脚真实电平；"松不开"=释放后读不到高 => 线被拉死或无上拉) */
uint8_t SW_I2C_LineCheck(void)
{
  /* SCL: 拉低应读到 0 */
  SCL(0);
  SW_I2C_Delay();
  if (SCL_READ() == GPIO_PIN_SET) { return 1; }

  /* SCL: 释放后靠上拉应读到 1 */
  SCL(1);
  SW_I2C_Delay();
  if (SCL_READ() == GPIO_PIN_RESET) { return 2; }

  /* SDA 同理 */
  SDA(0);
  SW_I2C_Delay();
  if (SDA_READ() == GPIO_PIN_SET) { return 3; }

  SDA(1);
  SW_I2C_Delay();
  if (SDA_READ() == GPIO_PIN_RESET) { return 4; }

  return 0;
}

uint8_t SW_I2C_RecvByte(uint8_t ack)
{
  uint8_t val = 0;

  /* 释放 SDA，由从机驱动数据位 */
  SDA(1);
  for (uint8_t i = 0; i < 8; i++)
  {
    SCL(1);
    SW_I2C_Delay();
    val = (uint8_t)(val << 1) | (SDA_READ() ? 1u : 0u);
    SCL(0);
    SW_I2C_Delay();
  }

  /* 第 9 个时钟给出主机应答位 */
  SDA(ack ? 0 : 1);
  SCL(1);
  SW_I2C_Delay();
  SCL(0);
  SW_I2C_Delay();
  SDA(1);
  return val;
}
