/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    can_master.c
  * @brief   MP157 M4 CAN 主端(网关)模块实现(FDCAN2, 经典 CAN 2.0, 500k)
  *
  *          - 收: 零中断轮询 CANRxTask 每 10ms 清 FDCAN2 RX FIFO0(与 C8T6 从端同风格,
  *            本链路帧率为 1Hz 心跳 + 零星事件, 轮询足够; 若将来要中断/FromISR,
  *            FDCAN2_IT0 NVIC 已使能且优先级=3 ≤ configMAX_SYSCALL 边界, 可行)。
  *          - 发: HAL_FDCAN_AddMessageToTxFifoQ 提交到 TX FIFO; 统一在 Poll 里发送,
  *            单写者免锁; NART=DISABLE(与从端一致, 单发不重传, 网路隔离)。
  *          - 在线判定仅在主端做: 3s 无 0x210 → online=0(供 RPMSG 上报 A7)。
  *
  *          协议表见 c8t6/can.md。本文件只允许改 USER CODE 之间的代码。
  *          ⚠️ MP1 HAL 无 HAL_FDCAN_AddTxMessage(那是 MP2 的), 用 AddMessageToTxFifoQ。
  ******************************************************************************
  */
/* USER CODE END Header */
#include "can_master.h"
#include "fdcan.h"
#include "cmsis_os2.h"
#include <string.h>

/* 从端状态快照(主端维护; 目前仅在 CANRxTask 单写者, 读方为调试器/RPMSG)。
   作为全局开放, 便于调试器 live watch 直接观察。 */
CAN_MasterMonitor_t g_can_master_mon;

/* 调试器/外部可直接改写即触发的待发指令(单次) */
volatile uint8_t g_can_master_cmd_pending = 0u;
volatile uint8_t g_can_master_cmd_value   = 0u;

#if (CAN_MASTER_DEBUG_AUTO_GATE_MS > 0u)
/* 调试后门: 上次自动开闸指令发出时刻(ms), 上电 3s 后首发 */
static uint32_t s_dbg_gate_tick = 0u;
#endif

void CAN_Master_Init(void)
{
  FDCAN_FilterTypeDef f;
  HAL_StatusTypeDef   st;

  memset(&f, 0, sizeof(f));
  f.IdType       = FDCAN_STANDARD_ID;
  f.FilterIndex  = 0;
  f.FilterType   = FDCAN_FILTER_MASK;        /* 经典掩码: FilterID1=值, FilterID2=掩码 */
  f.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
  f.FilterID1    = CAN_MASTER_EVT_ID;        /* 值 0x200 */
  f.FilterID2    = 0x700u;                   /* 掩码: 高5位匹配 => 收 0x200~0x2FF(事件+心跳) */
  st = HAL_FDCAN_ConfigFilter(&hfdcan2, &f);
  if (st == HAL_OK)
  {
    st = HAL_FDCAN_ConfigGlobalFilter(&hfdcan2, FDCAN_REJECT, FDCAN_REJECT,
                                      FDCAN_FILTER_REMOTE, FDCAN_FILTER_REMOTE);
  }
  if (st == HAL_OK)
  {
    st = HAL_FDCAN_Start(&hfdcan2);
  }
  /* 失败不硬复位: online 维持 0, 后续 Poll 仍运行(不会收帧) */
}

uint8_t CAN_Master_SendCmd(uint8_t cmd)
{
  FDCAN_TxHeaderTypeDef tx;
  uint8_t data[8];
  HAL_StatusTypeDef st;

  memset(&tx, 0, sizeof(tx));
  memset(data, 0, sizeof(data));
  tx.Identifier  = CAN_MASTER_CMD_ID;
  tx.IdType      = FDCAN_STANDARD_ID;
  tx.TxFrameType = FDCAN_DATA_FRAME;
  tx.DataLength  = FDCAN_DLC_BYTES_8;   /* 经典 CAN 数据帧 DLC=8 */
  tx.FDFormat    = FDCAN_CLASSIC_CAN;
  data[0] = cmd;

  st = HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan2, &tx, data);
  if (st == HAL_OK)
  {
    g_can_master_mon.tx_ok_count++;
    return 0u;
  }
  g_can_master_mon.tx_err_count++;
  return 1u;
}

void CAN_Master_RequestCmd(uint8_t cmd)
{
  g_can_master_cmd_value   = cmd;
  g_can_master_cmd_pending = 1u;
}

void CAN_Master_Poll(void)
{
  uint32_t now = osKernelGetTickCount();

  /* ---- 调试: 采一次 TX FIFO 空闲级/填充级(判断 M4 帧有没有被 ACK 发完) ---- */
  {
    uint32_t free = HAL_FDCAN_GetTxFifoFreeLevel(&hfdcan2);
    g_can_master_mon.tx_fifo_free = free;
    g_can_master_mon.tx_fifo_fill = (free < 8u) ? (8u - free) : 0u;
  }

  /* ---- 调试后门: 每 N ms 自动发一次开闸指令(宏在 can_master.h, 置 0 关闭) ---- */
#if (CAN_MASTER_DEBUG_AUTO_GATE_MS > 0u)
  if ((now - s_dbg_gate_tick) >= CAN_MASTER_DEBUG_AUTO_GATE_MS)
  {
    s_dbg_gate_tick = now;
    (void)CAN_Master_SendCmd(CAN_CMD_OPEN_GATE);
  }
#endif

  /* ---- 发: 提交外部(调试器/RPMSG)请的指令, 单次 ---- */
  if (g_can_master_cmd_pending != 0u)
  {
    g_can_master_cmd_pending = 0u;
    (void)CAN_Master_SendCmd(g_can_master_cmd_value);
    g_can_master_cmd_value = 0u;
  }

  /* ---- 收: 轮询清空 FDCAN2 RX FIFO0(零中断) ---- */
  while (HAL_FDCAN_GetRxFifoFillLevel(&hfdcan2, FDCAN_RX_FIFO0) > 0u)
  {
    FDCAN_RxHeaderTypeDef rh;
    uint8_t data[8];

    if (HAL_FDCAN_GetRxMessage(&hfdcan2, FDCAN_RX_FIFO0, &rh, data) != HAL_OK)
    {
      break;
    }
    g_can_master_mon.rx_total++;   /* 只要进 FIFO0 就计(回环自测直接看这个)  */
    /* 帧校验(协议恒 DLC=8, 见 can.md §3.5 精神): 非标准帧/非 8 字节丢弃 */
    if ((rh.IdType != FDCAN_STANDARD_ID) || ((rh.DataLength >> 16) != 8u))
    {
      continue;
    }
    switch (rh.Identifier)
    {
      case CAN_MASTER_EVT_ID:   /* 0x200 事件: ev/lux(大端)/drop%/status */
        g_can_master_mon.last_ev      = data[0];
        g_can_master_mon.last_lux     = (uint16_t)((uint16_t)data[1] << 8) | data[2];
        g_can_master_mon.last_drop    = data[3];
        g_can_master_mon.slave_status = data[4];
        g_can_master_mon.last_ev_tick = now;
        g_can_master_mon.ev_count++;
        break;
      case CAN_MASTER_HB_ID:    /* 0x210 心跳: status/uptime */
        g_can_master_mon.slave_status = data[0];
        g_can_master_mon.last_uptime  = data[1];
        g_can_master_mon.last_hb_tick = now;
        g_can_master_mon.hb_count++;
        g_can_master_mon.online = 1u;   /* 收到心跳即在线 */
        break;
      default:
        break;
    }
  }

  /* ---- 在线判定(仅主端): 3s 无 0x210 → 离线 ---- */
  if ((g_can_master_mon.online != 0u) &&
      (now > CAN_MASTER_OFFLINE_MS) &&
      ((now - g_can_master_mon.last_hb_tick) > CAN_MASTER_OFFLINE_MS))
  {
    g_can_master_mon.online = 0u;
  }
}

void CAN_Master_GetMonitor(CAN_MasterMonitor_t *m)
{
  if (m != NULL)
  {
    memcpy(m, &g_can_master_mon, sizeof(*m));
  }
}
