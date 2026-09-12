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
  *          - 收到 0x200 事件 → 自动回 0x110/EVENT_ACK(kind=0x01)。
  *
  *          ⭐ 接口 v2(2026-09-11): 设备抽象 —— 本层只解析/保存语义量
  *          (事件码/语义参数/状态位/节点身份/版本)，**不保存 lux/drop% 等传感器
  *          原始值**；板级因此对"下位机用了什么传感器"零假设。
  *
  *          协议表见 c8t6/can.md。本文件只允许改 USER CODE 之间的代码。
  *          ⚠️ MP1 HAL 无 HAL_FDCAN_AddTxMessage(那是 MP2 的), 用 AddMessageToTxFifoQ。
  ******************************************************************************
  */
/* USER CODE END Header */
#include "can_master.h"
#include "fdcan.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"          /* taskENTER_CRITICAL: 保护"待发指令"三字节的读+清 */
#include "task.h"
#include <string.h>

/* 从端状态快照(主端维护; 目前仅在 CANRxTask 单写者, 读方为调试器/RPMSG)。
   作为全局开放, 便于调试器 live watch 直接观察。 */
CAN_MasterMonitor_t g_can_master_mon;

/* 调试器/外部可直接改写即触发的待发指令(单次) */
volatile uint8_t g_can_master_cmd_pending = 0u;
volatile uint8_t g_can_master_cmd_value   = 0u;
volatile uint8_t g_can_master_cmd_arg     = 0u;

/* 下行指令序号(0x100 d[2]); 从端在 0x110/CMD_ACK 里回显, 用于确认"这条命令被处理了"。
   只在成功入 FIFO 时自增, 失败不消耗序号。 */
static uint8_t s_cmd_seq = 0u;

/* RPMSG 桥收帧钩子(可空; 单写者: Rpmsg_Task 早期注册一次, 之后只读) */
static CAN_Master_EventHook_t s_evt_hook = NULL;

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

/* 发一帧 0x100 指令: cmd(1B) | arg(1B) | seq(1B) | rsv[5] */
uint8_t CAN_Master_SendCmd(uint8_t cmd, uint8_t arg)
{
  FDCAN_TxHeaderTypeDef tx;
  uint8_t data[8];
  HAL_StatusTypeDef st;
  uint8_t seq = s_cmd_seq;

  memset(&tx, 0, sizeof(tx));
  memset(data, 0, sizeof(data));
  tx.Identifier  = CAN_MASTER_CMD_ID;
  tx.IdType      = FDCAN_STANDARD_ID;
  tx.TxFrameType = FDCAN_DATA_FRAME;
  tx.DataLength  = FDCAN_DLC_BYTES_8;   /* 经典 CAN 数据帧 DLC=8 */
  tx.FDFormat    = FDCAN_CLASSIC_CAN;
  data[0] = cmd;
  data[1] = arg;
  data[2] = seq;

  st = HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan2, &tx, data);
  if (st == HAL_OK)
  {
    s_cmd_seq = (uint8_t)(seq + 1u);     /* 只在真正发出时消耗序号 */
    g_can_master_mon.tx_ok_count++;
    return 0u;
  }
  g_can_master_mon.tx_err_count++;
  return 1u;
}

/* 回一帧 0x110 回执(kind|code|arg|rsv[5]):
   主端每收到一帧 0x200 自动回 kind=CAN_ACK_KIND_EVENT/d[1]=事件码(从端据此点 OLED)。
   从端**不产生**指令回执(v2 定稿删除: 0x110 在主→从段, 从端发出去无人接收)。 */
uint8_t CAN_Master_SendAck(uint8_t kind, uint8_t code, uint8_t arg)
{
  FDCAN_TxHeaderTypeDef tx;
  uint8_t data[8];
  HAL_StatusTypeDef st;

  memset(&tx, 0, sizeof(tx));
  memset(data, 0, sizeof(data));
  tx.Identifier  = CAN_MASTER_ACK_ID;
  tx.IdType      = FDCAN_STANDARD_ID;
  tx.TxFrameType = FDCAN_DATA_FRAME;
  tx.DataLength  = FDCAN_DLC_BYTES_8;   /* 经典 CAN 数据帧 DLC=8 */
  tx.FDFormat    = FDCAN_CLASSIC_CAN;
  data[0] = kind;
  data[1] = code;
  data[2] = arg;

  st = HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan2, &tx, data);
  if (st == HAL_OK)
  {
    g_can_master_mon.tx_ok_count++;
    return 0u;
  }
  g_can_master_mon.tx_err_count++;
  return 1u;
}

void CAN_Master_RequestCmd(uint8_t cmd, uint8_t arg)
{
  g_can_master_cmd_value   = cmd;
  g_can_master_cmd_arg     = arg;
  g_can_master_cmd_pending = 1u;
}

void CAN_Master_SetEventHook(CAN_Master_EventHook_t fn)
{
  s_evt_hook = fn;
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
    (void)CAN_Master_SendCmd(CAN_CMD_GATE_OPEN, 0u);
  }
#endif

  /* ---- 发: 提交外部(RPMSG/调试器)请的指令, 单次 ----
     读 cmd/arg 与清 pending 必须原子: 写方(Rpmsg_Task, 与 CANRxTask 同优先级时间片)
     在 value/arg 之后置 pending=1, 若被抢占就会出现"发了旧指令/丢了新指令/发出 cmd=0"。
     临界区内不做 HAL 调用, 极短。 */
  if (g_can_master_cmd_pending != 0u)
  {
    uint8_t cmd;
    uint8_t arg;

    taskENTER_CRITICAL();
    cmd = g_can_master_cmd_value;
    arg = g_can_master_cmd_arg;
    g_can_master_cmd_pending = 0u;
    taskEXIT_CRITICAL();

    (void)CAN_Master_SendCmd(cmd, arg);
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
    /* 事件钩子(rpmsg_bridge 注册): 每解析出一帧 0x200/0x210 即回调一次 */
    if (s_evt_hook != NULL)
    {
      s_evt_hook(rh.Identifier, (uint8_t)(rh.DataLength >> 16), data, now);
    }
    switch (rh.Identifier)
    {
      case CAN_MASTER_EVT_ID:   /* 0x200 事件: ev|arg(大端)|status|tick(大端) */
        g_can_master_mon.last_ev      = data[0];
        g_can_master_mon.last_arg     = (uint16_t)(((uint16_t)data[1] << 8) | data[2]);
        g_can_master_mon.slave_status = data[3];
        g_can_master_mon.last_ev_tick = now;
        g_can_master_mon.ev_count++;
        (void)CAN_Master_SendAck(CAN_ACK_KIND_EVENT, data[0], 0u);  /* 事件确认 */
        break;
      case CAN_MASTER_HB_ID:    /* 0x210 心跳: status|uptime_ms(大端)|dev_type|fw_ver|node_id */
        g_can_master_mon.slave_status   = data[0];
        g_can_master_mon.last_uptime_ms = ((uint32_t)data[1] << 24) |
                                          ((uint32_t)data[2] << 16) |
                                          ((uint32_t)data[3] << 8)  |
                                          ((uint32_t)data[4]);
        g_can_master_mon.dev_type       = data[5];
        g_can_master_mon.fw_ver         = data[6];
        g_can_master_mon.node_id        = data[7];
        g_can_master_mon.last_hb_tick   = now;
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
