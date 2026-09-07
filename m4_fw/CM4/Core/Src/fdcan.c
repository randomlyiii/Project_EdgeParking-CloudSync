/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    fdcan.c
  * @brief   This file provides code for the configuration
  *          of the FDCAN instances.
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
/* Includes ------------------------------------------------------------------*/
#include "fdcan.h"

/* USER CODE BEGIN 0 */
/* ⚠️ 位时序/发送队列(与 c8t6/can.md §3/§5 对齐, 与 .ioc 同步)：
 *   - 本板 FDCAN 内核时钟有两种环境实测值(2026-09-08):
 *     ① CubeIDE/工程模式: SystemClock_Config 开 PLL3Q → FDCAN=100MHz(.ioc RCC 段);
 *     ② Linux(A7)引导/remoteproc: 内核时钟树默认 → FDCAN=62.5MHz(实测)。
 *     → 位时序不写死: main.c 在 MX_FDCAN2_Init 后调 FDCAN2_AutotuneBitTiming()
 *       实测时钟并自动换算 500k(见本文件尾部); 此处静态值(.ioc 同步)只作
 *       "工程模式 100MHz 默认 + 实测失败兜底" = Prescaler25/(1+5+2)=8tq/采样75%。
 *   - AutoRetransmission=DISABLE = 单发不重传(与 C8T6 从端一致, 防无 ACK 反复重传)。
 *   - TxFifoQueueElmtsNbr 必须 >0，否则 HAL_FDCAN_AddMessageToTxFifoQ 无从发送。
 *   CubeMX 若再生成需确认以上未被回退(.ioc 已同步 FDCAN2)。 */
/* USER CODE END 0 */

FDCAN_HandleTypeDef hfdcan2;

/* FDCAN2 init function */
void MX_FDCAN2_Init(void)
{

  /* USER CODE BEGIN FDCAN2_Init 0 */

  /* USER CODE END FDCAN2_Init 0 */

  /* USER CODE BEGIN FDCAN2_Init 1 */

  /* USER CODE END FDCAN2_Init 1 */
  hfdcan2.Instance = FDCAN2;
  hfdcan2.Init.FrameFormat = FDCAN_FRAME_CLASSIC;
  hfdcan2.Init.Mode = FDCAN_MODE_NORMAL;
  hfdcan2.Init.AutoRetransmission = DISABLE;
  hfdcan2.Init.TransmitPause = DISABLE;
  hfdcan2.Init.ProtocolException = DISABLE;
  hfdcan2.Init.NominalPrescaler = 25;
  hfdcan2.Init.NominalSyncJumpWidth = 1;
  hfdcan2.Init.NominalTimeSeg1 = 5;
  hfdcan2.Init.NominalTimeSeg2 = 2;
  hfdcan2.Init.DataPrescaler = 1;
  hfdcan2.Init.DataSyncJumpWidth = 1;
  hfdcan2.Init.DataTimeSeg1 = 1;
  hfdcan2.Init.DataTimeSeg2 = 1;
  hfdcan2.Init.MessageRAMOffset = 0;
  hfdcan2.Init.StdFiltersNbr = 4;
  hfdcan2.Init.ExtFiltersNbr = 0;
  hfdcan2.Init.RxFifo0ElmtsNbr = 16;
  hfdcan2.Init.RxFifo0ElmtSize = FDCAN_DATA_BYTES_8;
  hfdcan2.Init.RxFifo1ElmtsNbr = 0;
  hfdcan2.Init.RxFifo1ElmtSize = FDCAN_DATA_BYTES_8;
  hfdcan2.Init.RxBuffersNbr = 0;
  hfdcan2.Init.RxBufferSize = FDCAN_DATA_BYTES_8;
  hfdcan2.Init.TxEventsNbr = 0;
  hfdcan2.Init.TxBuffersNbr = 0;
  hfdcan2.Init.TxFifoQueueElmtsNbr = 8;
  hfdcan2.Init.TxFifoQueueMode = FDCAN_TX_FIFO_OPERATION;
  hfdcan2.Init.TxElmtSize = FDCAN_DATA_BYTES_8;
  if (HAL_FDCAN_Init(&hfdcan2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN FDCAN2_Init 2 */

  /* USER CODE END FDCAN2_Init 2 */

}

void HAL_FDCAN_MspInit(FDCAN_HandleTypeDef* fdcanHandle)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};
  if(fdcanHandle->Instance==FDCAN2)
  {
  /* USER CODE BEGIN FDCAN2_MspInit 0 */

  /* USER CODE END FDCAN2_MspInit 0 */
  if(IS_ENGINEERING_BOOT_MODE())
  {

  /** Initializes the peripherals clock
  */
    PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_FDCAN;
    PeriphClkInit.FdcanClockSelection = RCC_FDCANCLKSOURCE_PLL3;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
    {
      Error_Handler();
    }

  }

    /* FDCAN2 clock enable */
    __HAL_RCC_FDCAN_CLK_ENABLE();

    __HAL_RCC_GPIOB_CLK_ENABLE();
    /**FDCAN2 GPIO Configuration
    PB5     ------> FDCAN2_RX
    PB6     ------> FDCAN2_TX
    */
    GPIO_InitStruct.Pin = GPIO_PIN_5;
    GPIO_InitStruct.Mode = GPIO_MODE_AF;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Alternate = GPIO_AF9_FDCAN2;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_6;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    GPIO_InitStruct.Alternate = GPIO_AF9_FDCAN2;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* FDCAN2 interrupt Init */
    HAL_NVIC_SetPriority(FDCAN2_IT0_IRQn, 3, 0);
    HAL_NVIC_EnableIRQ(FDCAN2_IT0_IRQn);
  /* USER CODE BEGIN FDCAN2_MspInit 1 */

  /* USER CODE END FDCAN2_MspInit 1 */
  }
}

void HAL_FDCAN_MspDeInit(FDCAN_HandleTypeDef* fdcanHandle)
{

  if(fdcanHandle->Instance==FDCAN2)
  {
  /* USER CODE BEGIN FDCAN2_MspDeInit 0 */

  /* USER CODE END FDCAN2_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_FDCAN_CLK_DISABLE();

    /**FDCAN2 GPIO Configuration
    PB5     ------> FDCAN2_RX
    PB6     ------> FDCAN2_TX
    */
    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_5|GPIO_PIN_6);

    /* FDCAN2 interrupt Deinit */
    HAL_NVIC_DisableIRQ(FDCAN2_IT0_IRQn);
  /* USER CODE BEGIN FDCAN2_MspDeInit 1 */

  /* USER CODE END FDCAN2_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */

/* ---- FDCAN 时钟实测 + 500k 位时序自动换算(2026-09-08) ----
   背景: 同一块板两种运行环境 FDCAN 内核时钟不同——
   工程模式(CubeIDE)=PLL3Q 100MHz(.ioc); Linux 引导/remoteproc=62.5MHz(实测)。
   静态位时序无法两全, 故启动时实测一次, 自动换算出 500k 的 Prescaler/Seg1/Seg2。
   用法: main.c 在 MX_FDCAN2_Init() 之后、CAN_Master_Init()(Start)之前调一次。
   实测失败时保持 .ioc 静态默认(100MHz 工程模式配置), 不影响工程模式。 */

volatile uint32_t g_fdcan_meas_hz = 0u;   /* 实测 FDCAN 内核时钟(Hz), 调试器 live watch */
volatile uint32_t g_fdcan_cfg_pre  = 0u;  /* 自动换算 Prescaler */
volatile uint32_t g_fdcan_cfg_seg1 = 0u;  /* 自动换算 TimeSeg1 */
volatile uint32_t g_fdcan_cfg_seg2 = 0u;  /* 自动换算 TimeSeg2 */

void FDCAN2_AutotuneBitTiming(void)
{
  uint32_t fclk, ttq, q = 0u, p = 0u;

  fclk = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_FDCAN);
  g_fdcan_meas_hz = fclk;
  ttq = (fclk >= 500000u) ? (fclk / 500000u) : 0u;  /* 每位所需 tq 数 */

  if ((ttq == 0u) || (ttq > (25u * 1024u)))
  {
    return;                       /* 测不到/离谱 → 用 .ioc 静态默认 */
  }

  for (uint32_t qq = 5u; qq <= 25u; qq++)           /* 优先整除组合 */
  {
    if ((ttq % qq) == 0u) { q = qq; p = ttq / qq; break; }
  }
  if (q == 0u)                                       /* 无整除 → 取误差最小近似 */
  {
    uint32_t emin = 0xFFFFFFFFu;
    for (uint32_t qq = 5u; qq <= 25u; qq++)
    {
      uint32_t pp = (ttq + qq / 2u) / qq;
      uint32_t err;
      if (pp < 1u) pp = 1u;
      err = (pp * qq > ttq) ? (pp * qq - ttq) : (ttq - pp * qq);
      if (err < emin) { emin = err; q = qq; p = pp; }
    }
  }

  if ((q < 5u) || (q > 25u) || (p < 1u) || (p > 1024u))
  {
    return;
  }

  {
    uint32_t s1 = (4u * q + 2u) / 5u - 1u;           /* 采样点约 70~80% */
    uint32_t s2;
    if (s1 < 1u) s1 = 1u;
    if (s1 > (q - 2u)) s1 = q - 2u;
    s2 = q - 1u - s1;
    hfdcan2.Init.NominalPrescaler      = p;
    hfdcan2.Init.NominalSyncJumpWidth  = 1u;
    hfdcan2.Init.NominalTimeSeg1       = s1;
    hfdcan2.Init.NominalTimeSeg2       = s2;
    g_fdcan_cfg_pre  = p;
    g_fdcan_cfg_seg1 = s1;
    g_fdcan_cfg_seg2 = s2;

    HAL_FDCAN_DeInit(&hfdcan2);       /* 未 Start, 重入安全 */
    (void)HAL_FDCAN_Init(&hfdcan2);   /* 按换算后的位时序重新初始化 */
  }
}

/* USER CODE END 1 */

