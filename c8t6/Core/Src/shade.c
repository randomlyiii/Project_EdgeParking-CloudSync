/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    shade.c
  * @brief   遮光检测状态机实现：百分比掉点 + 基线 EMA + 回滞去抖
  *
  *          状态机在 BH1750_Task(200ms) 每周期驱动一次：
  *          - 读数有效 -> Shade_FSM_Update(lux)；返回 ±1 边沿时由任务发 0x200；
  *          - 读数失败 -> Shade_ReportFault(1)，本周期不进状态机(不污染基线)；
  *            恢复读数后自动回正常(故障标志每周期由任务刷新)。
  *
  *          调参(掉点阈值/去抖次数)在 config.h，本文件不改数值。
  ******************************************************************************
  */
/* USER CODE END Header */
#include "shade.h"
#include "config.h"

/* 状态机私有状态(仅 BH1750_Task 单写者，无需互斥；其余任务只读) */
static ShadeState_t s_state    = SHADE_CLEAR;
static float        s_baseline = -1.0f;   /* -1=未初始化，首采样直接赋值 */
static uint8_t      s_confirm  = 0u;
static uint16_t     s_lux_last = 0u;      /* 最近一次有效 lux */
static uint8_t      s_drop     = 0u;      /* 当前 drop%(0~100) */
static uint8_t      s_fault    = 0u;      /* 最近一次采样是否故障 */

int8_t Shade_FSM_Update(uint16_t lux)
{
  int32_t drop = 0;

  s_lux_last = lux;
  s_fault    = 0u;

  /* 首次有效采样：直接建基线，不做判定 */
  if (s_baseline < 0.0f)
  {
    s_baseline = (float)lux;
    return 0;
  }

  if (s_state == SHADE_CLEAR)
  {
    /* 基线跟踪只在 CLEAR 态做：变亮快速跟上，慢漂移 EMA 平滑 */
    if ((float)lux >= s_baseline)
    {
      s_baseline = (float)lux;
    }
    else
    {
      s_baseline = (s_baseline * 15.0f + (float)lux) / 16.0f;
    }
  }
  /* SHADED 态基线冻结：遮住期间不塌陷，恢复判定只看当前 lux 与旧基线 */

  /* 掉点百分比(整数) */
  if ((s_baseline > 0.0f) && ((float)lux < s_baseline))
  {
    drop = (int32_t)((s_baseline - (float)lux) * 100.0f / s_baseline);
  }
  s_drop = (uint8_t)((drop > 100) ? 100 : ((drop < 0) ? 0 : drop));

  /* 去抖判定：进入用 DROP 阈值，释放用 RELEASE 阈值(回滞) */
  {
    uint32_t th  = (s_state == SHADE_CLEAR) ? SHADE_DROP_PERCENT : SHADE_RELEASE_PERCENT;
    uint8_t  hit = (s_state == SHADE_CLEAR)
                     ? ((uint32_t)s_drop >= th)
                     : ((uint32_t)s_drop <  th);
    if (hit)
    {
      if (++s_confirm >= SHADE_CONFIRM_N)
      {
        s_confirm = 0u;
        s_state   = (s_state == SHADE_CLEAR) ? SHADE_SHADED : SHADE_CLEAR;
        return (s_state == SHADE_SHADED) ? 1 : -1;   /* 边沿：1 进入遮光 / -1 恢复 */
      }
    }
    else
    {
      s_confirm = 0u;
    }
  }
  return 0;
}

void Shade_ReportFault(uint8_t fault)
{
  s_fault = (fault != 0u) ? 1u : 0u;
}

uint8_t Shade_IsShaded(void)
{
  return (s_state == SHADE_SHADED) ? 1u : 0u;
}

uint8_t Shade_GetDrop(void)
{
  return s_drop;
}

uint16_t Shade_GetLuxLast(void)
{
  return s_lux_last;
}

uint8_t Shade_SensorFault(void)
{
  return s_fault;
}
