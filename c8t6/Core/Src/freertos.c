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
#include "bh1750.h"
#include "ssd1306.h"
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
/* OLED 显示布局(8x16 字体, 4 行 x 16 列, 行/列均从 1 起):
 *   行1: 标题/状态   行2: Lux 光照   行3: Gate 道闸   行4: CAN 链路(预留)
 * Show* 只写影子显存，改完调 SSD1306_UpdateScreen() 上屏；
 * 所有对 OLED 的访问先拿 OledMutex，后续任务(如 SG90/CAN)要刷屏直接复用。
 */
osMutexId_t OledMutexHandle;

/* OLED 各显示行号(8x16 字体) */
#define OLED_LINE_TITLE   1u
#define OLED_LINE_LUX     2u
#define OLED_LINE_GATE    3u
#define OLED_LINE_CAN     4u
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
  .stack_size = 256 * 4,
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
  /* 诊断结果 LED 编码(定义在 main.c，splash 阶段已测完)，循环闪 N 下+停顿:
       1=SCL拉不低(引脚控制异常,固件层)      2=SCL松不开(线被拉死/无上拉)
       3=SDA拉不低                            4=SDA松不开(同上)
       5=线正常但 0x78 无ACK(模块没挂上/接线) 6=0x78 有ACK(模块在总线!)
     闪烁本身同时证明内核与任务在运行 */
  extern volatile uint8_t g_DiagCode;

  GPIO_InitTypeDef led = {0};
  __HAL_RCC_GPIOC_CLK_ENABLE();
  led.Pin   = GPIO_PIN_13;
  led.Mode  = GPIO_MODE_OUTPUT_PP;
  led.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &led);

  /* Infinite loop */
  for (;;)
  {
    for (uint8_t i = 0; i < g_DiagCode; i++)
    {
      HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
      osDelay(60);
      HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
      osDelay(260);
    }
    osDelay(1600);
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
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
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
  /* 临时：OLED 点屏测试阶段挂起本任务，避免与 OLED 并发占用 I2C1。
     OLED 验证通过后恢复下面的读取逻辑。 */
  osThreadSuspend(NULL);

  uint16_t lux = BH1750_ERR_VALUE;
  osStatus_t res;

  /* 连续 H 分辨率模式：一次配置后传感器自动循环测量(约180ms/次) */
  if (BH1750_Init() != HAL_OK)
  {
    /* 传感器不在总线/初始化失败，上报哨兵值让 OLED 显示 ERR */
    lux = BH1750_ERR_VALUE;
    osMessageQueuePut(BhDataQueueHandle, &lux, 0, osWaitForever);
  }

  /* Infinite loop */
  for (;;)
  {
    osDelay(200);

    if (BH1750_ReadLux(&lux) != HAL_OK)
    {
      lux = BH1750_ERR_VALUE;
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
  /* 当前阶段只有 OLED：循环里每秒重写 HelloWorld
     (8x16 大字体，第 1 行第 1 列，与 Keil 参考工程同款位置/字体) */
  if (SSD1306_Init() != HAL_OK)
  {
    /* OLED 不在总线：本节点失去显示能力，任务自挂起，不拖累其它任务 */
    osThreadSuspend(NULL);
  }

  /* Infinite loop */
  for (;;)
  {
    osMutexAcquire(OledMutexHandle, osWaitForever);
    SSD1306_Fill(0x00);
    SSD1306_ShowString(OLED_LINE_TITLE, 1, "HelloWorld");
    SSD1306_UpdateScreen();
    osMutexRelease(OledMutexHandle);

    osDelay(1000);
  }
  /* USER CODE END OLEDTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */

