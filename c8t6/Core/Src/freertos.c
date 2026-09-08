/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
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
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "i2c.h"
#include "config.h"      /* 遮光/CAN/SG90/OLED 全局常量(现场调参只改这里) */
#include "bh1750.h"
#include "ssd1306.h"
#include "shade.h"
#include "can_node.h"
#include "gate.h"
#include <stdio.h>
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */
/* OLED 显示布局(8x16 字体, 4 行 x 16 列, 行/列均从 1 起; 行号宏在 config.h):
 *   行1: 标题       行2: Lux+drop%     行3: Gate 道闸     行4: CAN 链路
 * Show* 只写影子显存，改完调 SSD1306_UpdateScreen() 上屏；
 * 所有对 OLED 的访问先拿 OledMutex，后续任务(如 CAN)要刷屏直接复用。 */
/* 软件 I2C 总线互斥(PB8/PB9)：OLED 与 BH1750 两个任务都 bit-bang 同一条
 * 总线，事务级串行化——谁持锁谁独占总线，持锁期间可连续多条 Start/Stop。 */
osMutexId_t OledMutexHandle;

/* CAN 发送互斥：BH1750_Task(事件 0x200) 与 CAN_Rx_Task(心跳 0x210/查询应答)
 * 并发调用 HAL_CAN_AddTxMessage，用本锁串行化邮箱选取(can_node.c 使用)。 */
osMutexId_t CanTxMutexHandle;
/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for CAN_Rx_Task */
osThreadId_t CAN_Rx_TaskHandle;
const osThreadAttr_t CAN_Rx_Task_attributes = {
  .name = "CAN_Rx_Task",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityHigh,
};
/* Definitions for BH1750_Task */
osThreadId_t BH1750_TaskHandle;
const osThreadAttr_t BH1750_Task_attributes = {
  .name = "BH1750_Task",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for OLED_Task */
osThreadId_t OLED_TaskHandle;
const osThreadAttr_t OLED_Task_attributes = {
  .name = "OLED_Task",
  .stack_size = 384 * 4,   /* oled_standard.md 6.3: sprintf+I2C 刷屏任务实测 >=384 words
                              (与 c8t6.ioc 的 OLED_Task 栈值同步改，regen 后需确认保留) */
  .priority = (osPriority_t) osPriorityLow,
};
/* Definitions for BhDataQueue */
osMessageQueueId_t BhDataQueueHandle;
const osMessageQueueAttr_t BhDataQueue_attributes = {
  .name = "BhDataQueue"
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);
void CANRxTask(void *argument);
void BH1750Task(void *argument);
void OLEDTask(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  OledMutexHandle = osMutexNew(NULL);
  CanTxMutexHandle = osMutexNew(NULL);
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* Create the queue(s) */
  /* creation of BhDataQueue */
  BhDataQueueHandle = osMessageQueueNew (8, sizeof(uint16_t), &BhDataQueue_attributes);

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* creation of CAN_Rx_Task */
  CAN_Rx_TaskHandle = osThreadNew(CANRxTask, NULL, &CAN_Rx_Task_attributes);

  /* creation of BH1750_Task */
  BH1750_TaskHandle = osThreadNew(BH1750Task, NULL, &BH1750_Task_attributes);

  /* creation of OLED_Task */
  OLED_TaskHandle = osThreadNew(OLEDTask, NULL, &OLED_Task_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN StartDefaultTask */
  /* PC13 心跳灯(1Hz)：证明内核与任务正常运行。
     (联调期的诊断闪码三件套已随"正常模式"收尾拆除，见 git 历史) */
  GPIO_InitTypeDef led = {0};
  __HAL_RCC_GPIOC_CLK_ENABLE();
  led.Pin   = GPIO_PIN_13;
  led.Mode  = GPIO_MODE_OUTPUT_PP;
  led.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &led);

  /* Infinite loop */
  for (;;)
  {
    HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
    osDelay(500);
  }
  /* USER CODE END StartDefaultTask */
}

/* USER CODE BEGIN Header_CANRxTask */
/**
* @brief Function implementing the CAN_Rx_Task thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_CANRxTask */
void CANRxTask(void *argument)
{
  /* USER CODE BEGIN CANRxTask */
  /* CAN 已在 main 裸机段 Init(过滤器+Start, can_node.c)。
     本任务每 10ms 轮询 FIFO0 收 0x100 指令/查询并顺带驱动 1Hz 心跳(0x210)；
     零中断轮询的理由见 can.md §软件架构(从站帧率低, 轮询足够)。 */
  /* Infinite loop */
  for(;;)
  {
    CAN_Node_Poll();
    osDelay(CAN_POLL_PERIOD_MS);
  }
  /* USER CODE END CANRxTask */
}

/* USER CODE BEGIN Header_BH1750Task */
/**
* @brief Function implementing the BH1750_Task thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_BH1750Task */
void BH1750Task(void *argument)
{
  /* USER CODE BEGIN BH1750Task */
  uint16_t lux = BH1750_ERR_VALUE;
  uint8_t  fail_streak = 0;
  uint8_t  read_ok;
  osStatus_t res;

  /* 初始化(阻塞约 200ms)：总线在 main 裸机段 splash 时已由 SSD1306_Init
     (内部 SW_I2C_Init)建好；传感器掉线/后插上由循环里的"连续失败重初始化"自愈 */
  osMutexAcquire(OledMutexHandle, osWaitForever);
  if (BH1750_Init() != HAL_OK)
  {
    lux = BH1750_ERR_VALUE;
    osMessageQueuePut(BhDataQueueHandle, &lux, 0, osWaitForever);
  }
  osMutexRelease(OledMutexHandle);

  /* Infinite loop */
  for (;;)
  {
    osDelay(BH1750_READ_PERIOD_MS);

    /* 持总线锁读数(与 OLED 刷屏互斥) */
    osMutexAcquire(OledMutexHandle, osWaitForever);
    read_ok = (BH1750_ReadLux(&lux) == HAL_OK);
    if (!read_ok)
    {
      lux = BH1750_ERR_VALUE;
      /* 连续失败约 5s：传感器可能刚上电/后插上，重发初始化序列 */
      if (++fail_streak >= BH1750_REINIT_FAILS)
      {
        fail_streak = 0;
        (void)BH1750_Init();
      }
    }
    else
    {
      fail_streak = 0;
    }
    osMutexRelease(OledMutexHandle);

    /* 遮光状态机 + CAN 上报(不需 I2C 总线，锁已释放):
       - 读失败: 只上报故障标志(状态位 bit2), 不喂状态机(防污染基线);
       - 读成功: 每周期喂 lux, 状态翻转边沿发 0x200 事件(1=遮光/车到位, -1=恢复)。 */
    if (read_ok)
    {
      int8_t edge;
      Shade_ReportFault(0u);
      edge = Shade_FSM_Update(lux);
      if (edge == 1)
      {
        (void)CAN_Node_SendEvent(CAN_EVT_SHADED, lux, Shade_GetDrop());
      }
      else if (edge == -1)
      {
        (void)CAN_Node_SendEvent(CAN_EVT_RECOVER, lux, Shade_GetDrop());
      }
    }
    else
    {
      Shade_ReportFault(1u);
    }

    /* 队列只留最新值：满了就先丢掉最旧的一条再放 */
    res = osMessageQueuePut(BhDataQueueHandle, &lux, 0, 0);
    if (res == osErrorResource)
    {
      uint16_t drop;
      osMessageQueueGet(BhDataQueueHandle, &drop, NULL, 0);
      osMessageQueuePut(BhDataQueueHandle, &lux, 0, 0);
    }
  }
  /* USER CODE END BH1750Task */
}

/* USER CODE BEGIN Header_OLEDTask */
/**
* @brief Function implementing the OLED_Task thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_OLEDTask */
void OLEDTask(void *argument)
{
  /* USER CODE BEGIN OLEDTask */
  uint16_t lux = BH1750_ERR_VALUE;
  uint16_t rx;
  char line[17];   /* 16 字符 + '\0'（oled_standard.md 6.2：严禁越 16 列） */

  /* 屏已在 main 裸机段点亮(splash)，此处重初始化一次保证状态干净；
     同样要拿总线锁——BH1750Task 可能正在初始化/读数 */
  osMutexAcquire(OledMutexHandle, osWaitForever);
  (void)SSD1306_Init();
  osMutexRelease(OledMutexHandle);

  /* Infinite loop */
  for (;;)
  {
    /* 抽干队列取最新光照值(含 0xFFFF=传感器故障哨兵) */
    while (osMessageQueueGet(BhDataQueueHandle, &rx, NULL, 0) == osOK)
    {
      lux = rx;
    }

    /* 正常模式界面(4 行 x 16 列, 8x16 字体; 行号宏在 config.h):
       行1 标题 | 行2 Lux+drop% | 行3 Gate 道闸 | 行4 CAN 链路状态 */
    {
      uint32_t now = osKernelGetTickCount();
      uint32_t ev_tick  = CAN_Node_LastEventTick();
      uint32_t ack_tick = CAN_Node_LastAckTick();
      const char *can_st;

      /* 行4 状态集: CAN:OK | CAN:EVT(事件已发未确认) | CAN:*OK*(★板子已回 0x110 确认)
         | CAN:ERR。tick=0 表示从未发生(启动时不误闪)。 */
      if (CAN_Node_TxError())
      {
        can_st = "CAN:ERR ";        /* 最近一次发送失败(邮箱满/无 ACK) */
      }
      else if ((ack_tick != 0u) && ((uint32_t)(now - ack_tick) < OLED_ACK_FLASH_MS))
      {
        can_st = "CAN:Sended";        /* ★ 特殊标识: 板子(M4)已回 0x110 确认收到光敏事件 */
      }
      else if ((ev_tick != 0u) && ((uint32_t)(now - ev_tick) < OLED_EVT_FLASH_MS))
      {
        can_st = "CAN:EVT ";        /* 光敏事件已发出, 等待/未确认窗口 */
      }
      else
      {
        can_st = "CAN:OK  ";
      }

      osMutexAcquire(OledMutexHandle, osWaitForever);
      SSD1306_Fill(0x00);

      /* 行1: 标题(正式名, 见 config.h OLED_TITLE_STR) */
      SSD1306_ShowString(OLED_LINE_TITLE, 1, OLED_TITLE_STR);

      /* 行2: 光照 + 掉点(BH1750; 故障显示 Lux:ERR) */
      if (lux == BH1750_ERR_VALUE)
      {
        snprintf(line, sizeof(line), "Lux:ERR  D:--");
      }
      else
      {
        snprintf(line, sizeof(line), "Lux:%u D:%u%%",
                 (unsigned int)lux, (unsigned int)Shade_GetDrop());
      }
      SSD1306_ShowString(OLED_LINE_LUX, 1, line);

      /* 行3: 道闸执行状态(真实到位; 缓动途中显示 MOVE) */
      if (Gate_IsMoving())
      {
        snprintf(line, sizeof(line), "Gate:MOVE");
      }
      else
      {
        snprintf(line, sizeof(line), "Gate:%s", Gate_IsOpen() ? "OPEN " : "CLOSE");
      }
      SSD1306_ShowString(OLED_LINE_GATE, 1, line);

      /* 行4: CAN 链路 */
      SSD1306_ShowString(OLED_LINE_CAN, 1, can_st);

      SSD1306_UpdateScreen();
      osMutexRelease(OledMutexHandle);
    }

    osDelay(OLED_REFRESH_MS);
  }
  /* USER CODE END OLEDTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */

