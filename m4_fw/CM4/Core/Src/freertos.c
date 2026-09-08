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
#include "FreeRTOS.h"
#include "cmsis_os2.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "can_master.h"
#include "rpmsg_bridge.h"
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
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for Rpmsg_Task(RPMSG 网关, 第3步; 栈尺寸见 rpmsg_bridge.h) */
osThreadId_t Rpmsg_TaskHandle;
const osThreadAttr_t Rpmsg_Task_attributes = {
  .name = "Rpmsg_Task",
  .stack_size = RPMSG_BRIDGE_TASK_STACK * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);
void CANRxTask(void *argument);

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
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* creation of CAN_Rx_Task */
  CAN_Rx_TaskHandle = osThreadNew(CANRxTask, NULL, &CAN_Rx_Task_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* 第3步: RPMSG 网关任务(OpenAMP 单任务模型, 收发/心跳全在其内, 见 rpmsg_bridge.c) */
  Rpmsg_TaskHandle = osThreadNew(Rpmsg_Task, NULL, &Rpmsg_Task_attributes);
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
  /* 业务指示灯(底板 LED_GREEN=PA10, 低电平亮):
     C8T6 在线(g_can_master_mon.online, CANRxTask 维护) → 1Hz 心跳(固件活+链路通);
     离线(3s 无 0x210 心跳) → ~5Hz 快闪告警。
     引脚出自 100ASK 官方 01_LED 例程。 */
  GPIO_InitTypeDef gpio = {0};
  __HAL_RCC_GPIOA_CLK_ENABLE();
  gpio.Pin   = GPIO_PIN_10;
  gpio.Mode  = GPIO_MODE_OUTPUT_PP;
  gpio.Pull  = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &gpio);
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_10, GPIO_PIN_SET);   /* 先灭 */

  /* Infinite loop */
  for (;;)
  {
    uint8_t online = g_can_master_mon.online;
    HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_10);
    osDelay(online ? 500u : 100u);   /* 在线 1Hz 心跳; 离线 5Hz 快闪 */
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
  /* CAN 已在 main 裸机段 Init(过滤器+Start, can_master.c)。
     本任务每 10ms 轮询 FDCAN2 RX FIFO0: 收 0x200 事件/0x210 心跳 → 更新监视快照;
     提交外部(调试器/RPMSG)请求的 0x100 指令; 3s 无心跳 → online=0。
     零中断轮询理由见 can_master.c 头注(can.md §5.2 同风格)。 */
  for(;;)
  {
    CAN_Master_Poll();
    osDelay(CAN_MASTER_POLL_MS);
  }
  /* USER CODE END CANRxTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */

