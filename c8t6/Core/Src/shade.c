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
  *          调参(档位阈值表/去抖次数)在 config.h，本文件不改数值。
  *          产出的 lux/drop% 只给本节点 OLED，不上 CAN(接口 v2 语义事件)。
  ******************************************************************************
  */
/* USER CODE END Header */
#include "shade.h"
#include "config.h"

/* 状态机私有状态(仅 BH1750_Task 单写者，无需互斥；其余任务只读)
   s_level 例外: 由 CAN_Rx_Task 经 Shade_SetDetectLevel() 写, 8 位读写天然原子。 */
static ShadeState_t s_state    = SHADE_CLEAR;
static float        s_baseline = -1.0f;   /* -1=未初始化，首采样直接赋值 */
static uint8_t      s_confirm  = 0u;
static uint16_t     s_lux_last = 0u;      /* 最近一次有效 lux */
static uint8_t      s_drop     = 0u;      /* 当前 drop%(0~100) */
static uint8_t      s_fault    = 0u;      /* 最近一次采样是否故障 */
static uint8_t      s_level    = SHADE_LEVEL_DEFAULT;   /* 灵敏度档位 1..5 */

/* 档位 1..5 -> 掉点阈值 %(表在 config.h，换传感器只改那张表) */
static const uint8_t s_level_drop[] = SHADE_LEVEL_DROP_TABLE;
#define SHADE_LEVEL_TABLE_LEN  (sizeof(s_level_drop) / sizeof(s_level_drop[0]))

uint8_t Shade_GetDetectLevel(void)
{
  return s_level;
}

uint8_t Shade_GetDropThreshold(void)
{
  uint8_t idx = (uint8_t)((s_level >= SHADE_LEVEL_MIN) ? (s_level - SHADE_LEVEL_MIN) : 0u);
  if (idx >= (uint8_t)SHADE_LEVEL_TABLE_LEN)
  {
    idx = (uint8_t)(SHADE_LEVEL_TABLE_LEN - 1u);
  }
  return s_level_drop[idx];
}

uint8_t Shade_SetDetectLevel(uint8_t level)
{
  if ((level < SHADE_LEVEL_MIN) || (level > SHADE_LEVEL_MAX) ||
      (level > SHADE_LEVEL_TABLE_LEN))
  {
    return 1u;   /* 越界: 保持原档位 */
  }
  s_level = level;   /* 单字节写, 与 BH1750_Task 的读天然原子;
                        故意**不动** s_confirm(它是 BH1750_Task 单写者, 跨任务改写会丢更新);
                        代价只是换档后可能提前一拍翻转, 无功能影响 */
  return 0u;
}

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

  /* 去抖判定：进入用当前档位阈值，释放用 阈值/回滞系数(避免临界抖动) */
  {
    uint32_t drop_th = (uint32_t)Shade_GetDropThreshold();
    uint32_t rel_th  = drop_th / (uint32_t)SHADE_RELEASE_DIV;
    uint32_t th      = (s_state == SHADE_CLEAR) ? drop_th : rel_th;
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
