/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    can_node.c
  * @brief   C8T6 CAN 从节点模块实现(bxCAN1, 经典 CAN 2.0, 500k, 标准帧)
  *
  *          收发架构(遵循 MD文档/can_standard.md §3.8/§4 的 FreeRTOS 经验):
  *          - 收: 零中断纯轮询——CAN_Rx_Task 每 10ms 清空 FIFO0。
  *            本从站对端只有 M4 一个主节点、帧率极低(指令零星 + 心跳 1Hz)，
  *            轮询绰绰有余且从根上避开 F1 的 CAN ISR/FreeRTOS 优先级坑；
  *            若将来指令突发量大，再改 FIFO0 中断+ISR 置标志方案(见 can.md)。
  *          - 发: CAN_TxFrame() 先查空闲邮箱再 AddTxMessage(NART=ENABLE 单发,
  *            失败即弃不占邮箱)，任务间用 CanTxMutex(freertos.c 创建)串行化。
  *          - 超时/离线判定只在主端(M4)做；从端不判主端(见 can.md §超时约定)。
  *
  *          字节级协议表见 c8t6/can.md；本文件只允许改 USER CODE 之间的代码。
  ******************************************************************************
  */
/* USER CODE END Header */
#include "can_node.h"
#include "can.h"        /* hcan(500k, NART=ENABLE, 见 can.c) */
#include "config.h"
#include "shade.h"
#include "FreeRTOS.h"
#include "cmsis_os.h"
#include <string.h>

/* freertos.c MX_FREERTOS_Init() 创建的 CAN 发送互斥(USER CODE RTOS_MUTEX 段) */
extern osMutexId_t CanTxMutexHandle;

/* 节点私有状态(单写者规则: s_gate_open 仅 Poll 写; s_tx_err 仅 CAN_TxFrame 写;
   s_last_event_tick 仅 SendEvent 写; 其余任务只读, 8/32 位读写天然原子) */
static volatile uint8_t  s_gate_open        = 0u;   /* 1=闸开 */
static volatile uint8_t  s_tx_err           = 0u;   /* 1=最近一次发送失败 */
static volatile uint32_t s_last_event_tick  = 0u;   /* 最近成功发 0x200 的 tick */
static volatile uint32_t s_hb_last_tick     = 0u;   /* 上次心跳 tick */

/* 调试监视(全局开放, 调试器 live watch)。volatile 防止优化器把"只写不读"的全局删掉 */
volatile CAN_NodeDbg_t g_can_node_dbg;

/* 单轮轮询最多处理的收帧数(与 MD文档/can_standard.md §4.4 一致):
   防对端异常洪泛时 CAN_Rx_Task(高优先)长时间占 CPU 饿死低优先任务；
   本端真实负载(心跳 1Hz + 零星指令)远达不到此上限。 */
#define CAN_RX_DRAIN_MAX  16u

/* 汇总 0x200 d[4] / 0x210 d[0] 状态字节(与两帧同源) */
static uint8_t StatusBits_Calc(void)
{
  uint8_t b = 0u;
  if (s_gate_open)          b |= CAN_STAT_GATE_OPEN;
  if (Shade_IsShaded())     b |= CAN_STAT_SHADED;
  if (Shade_SensorFault())  b |= CAN_STAT_LUX_FAULT;
  if (s_tx_err)             b |= CAN_STAT_CAN_ERR;
  return b;
}

/* 发送一帧标准数据帧(DLC=8)。返回 0=已提交邮箱。
   失败(无空闲邮箱/AddTx 失败)置状态位 bit3；成功清除(健康位语义)。
   调用方须处于任务上下文；内部用 CanTxMutex 串行化并发发送方。 */
static uint8_t CAN_TxFrame(uint32_t std_id, const uint8_t *data)
{
  CAN_TxHeaderTypeDef th;
  uint32_t mb;
  uint8_t  ret = 1u;

  if (CanTxMutexHandle != NULL)
  {
    osMutexAcquire(CanTxMutexHandle, osWaitForever);
  }

  if (HAL_CAN_GetTxMailboxesFreeLevel(&hcan) > 0u)
  {
    memset(&th, 0, sizeof(th));
    th.StdId = std_id;
    th.IDE   = CAN_ID_STD;
    th.RTR   = CAN_RTR_DATA;
    th.DLC   = 8u;
    if (HAL_CAN_AddTxMessage(&hcan, &th, (uint8_t *)data, &mb) == HAL_OK)
    {
      ret = 0u;   /* 已进邮箱。NART=ENABLE: 发送失败(无 ACK)时硬件放弃并释放邮箱 */
    }
  }

  if (CanTxMutexHandle != NULL)
  {
    osMutexRelease(CanTxMutexHandle);
  }

  if (ret == 0u)
  {
    g_can_node_dbg.tx_total++;
  }
  s_tx_err = ret;
  return ret;
}

/* 组一帧 0x200 载荷: event + lux(大端) + drop% + 状态位 */
static void CAN_BuildEvtPayload(uint8_t *d, uint8_t ev, uint16_t lux, uint8_t drop)
{
  d[0] = ev;
  d[1] = (uint8_t)(lux >> 8);     /* lux 大端: 高字节在前 */
  d[2] = (uint8_t)(lux & 0xFFu);
  d[3] = drop;
  d[4] = StatusBits_Calc();
  /* d[5..7] 预留, 恒 0 */
}

void CAN_Node_Init(void)
{
  CAN_FilterTypeDef f;
  HAL_StatusTypeDef st;

  memset(&f, 0, sizeof(f));
  /* 标准帧 11bit ID 在滤波器寄存器 [31:21]；高位半字偏移 5 位。
     掩码取 0x700<<5 => 只收 0x1xx 主→从指令段(当前 0x100, 预留扩展)。
     若想精确只收 0x100，掩码改 0x7FF<<5(见 can.md §过滤器)。 */
  f.FilterIdHigh         = (uint16_t)(CAN_CMD_ID << 5u);
  f.FilterIdLow          = 0u;
  f.FilterMaskIdHigh     = (uint16_t)(0x700u << 5u);   /* 只收 0x1xx 主→从指令段 */
  f.FilterMaskIdLow      = 0u;
  f.FilterFIFOAssignment = CAN_RX_FIFO0;
  f.FilterBank           = 0u;
  f.FilterMode           = CAN_FILTERMODE_IDMASK;
  f.FilterScale          = CAN_FILTERSCALE_32BIT;
  f.FilterActivation     = CAN_FILTER_ENABLE;

  st = HAL_CAN_ConfigFilter(&hcan, &f);
  if (st == HAL_OK)
  {
    st = HAL_CAN_Start(&hcan);   /* 有超时保护; 总线异常时不阻塞, 返回错误 */
  }
  if (st != HAL_OK)
  {
    s_tx_err = 1u;   /* 链路起不来 => OLED CAN:ERR / 状态位 bit3 */
  }
}

void CAN_Node_Poll(void)
{
  uint32_t now = osKernelGetTickCount();

  /* ---- 调试: 采一次 CAN_ESR(REC/TEC/LEC/BOFF), 供 live watch ----
     F1 的 CAN_ESR 位段(RM0008): REC=[23:16] TEC=[15:8] LEC=[6:4] BOFF=[2] */
  g_can_node_dbg.can_esr_raw = hcan.Instance->ESR;
  g_can_node_dbg.rec         = (g_can_node_dbg.can_esr_raw >> 16u) & 0xFFu;
  g_can_node_dbg.tec         = (g_can_node_dbg.can_esr_raw >> 8u)  & 0xFFu;
  g_can_node_dbg.lec         = (g_can_node_dbg.can_esr_raw >> 4u)  & 0x7u;
  g_can_node_dbg.boff        = (g_can_node_dbg.can_esr_raw >> 2u)  & 0x1u;

  /* ---- 收: 清空 FIFO0(零中断轮询; 每轮上限防饿死, 见文件头宏) ---- */
  {
    uint8_t drained = 0u;
    while ((drained < CAN_RX_DRAIN_MAX) &&
           (HAL_CAN_GetRxFifoFillLevel(&hcan, CAN_RX_FIFO0) > 0u))
    {
      CAN_RxHeaderTypeDef rh;
      uint8_t data[8];
      uint8_t d[8];

      if (HAL_CAN_GetRxMessage(&hcan, CAN_RX_FIFO0, &rh, data) != HAL_OK)
      {
        break;
      }
      drained++;
      g_can_node_dbg.rx_total++;   /* 只要进了 FIFO0 就计(无论是否解析) */

      /* 帧校验(正常模式, can.md 审计整改项): 只收标准帧 0x100 且 DLC=8,
        其余丢弃——硬件掩码过滤器(0x1xx 段)之外的二次软件校验。
        注: 联调期曾临时关闭放行异常帧, 收尾后恢复。 */
      if ((rh.IDE != CAN_ID_STD) || (rh.StdId != CAN_CMD_ID) || (rh.DLC != 8u))
      {
        continue;
      }
      switch (data[0])
      {
        case CAN_CMD_OPEN_GATE:
          s_gate_open = 1u;
          g_can_node_dbg.gate_opens++;
          break;
        case CAN_CMD_CLOSE_GATE:
          s_gate_open = 0u;
          break;
        case CAN_CMD_QUERY:   /* 查询: 回一帧 0x200 快照(载荷同事件帧) */
          memset(d, 0, sizeof(d));
          CAN_BuildEvtPayload(d,
                              Shade_IsShaded() ? CAN_EVT_SHADED : CAN_EVT_RECOVER,
                              Shade_GetLuxLast(), Shade_GetDrop());
          (void)CAN_TxFrame(CAN_EVT_ID, d);
          break;
        default:              /* 未知指令码: 忽略 */
          break;
      }
    }
  }

  /* ---- 发: 1Hz 心跳(0x210) ---- */
  if ((now >= CAN_HB_PERIOD_MS) && ((now - s_hb_last_tick) >= CAN_HB_PERIOD_MS))
  {
    uint8_t d[8];
    s_hb_last_tick = now;
    memset(d, 0, sizeof(d));
    d[0] = StatusBits_Calc();        /* 与 0x200 d[4] 同源 */
    d[1] = (uint8_t)(now / 1000u);   /* 上电秒数低 8 位(主端辅助判复位) */
    if (CAN_TxFrame(CAN_HB_ID, d) == 0u)
    {
      g_can_node_dbg.hb_sent++;
    }
  }
}

uint8_t CAN_Node_SendEvent(uint8_t ev, uint16_t lux, uint8_t drop)
{
  uint8_t d[8];

  memset(d, 0, sizeof(d));
  CAN_BuildEvtPayload(d, ev, lux, drop);
  if (CAN_TxFrame(CAN_EVT_ID, d) == 0u)
  {
    s_last_event_tick = osKernelGetTickCount();
    g_can_node_dbg.ev_sent++;
    return 0u;
  }
  return 1u;
}

uint8_t CAN_Node_GateOpen(void)
{
  return s_gate_open;
}

uint8_t CAN_Node_TxError(void)
{
  return s_tx_err;
}

uint32_t CAN_Node_LastEventTick(void)
{
  return s_last_event_tick;
}

uint8_t CAN_Node_StatusBits(void)
{
  return StatusBits_Calc();
}
