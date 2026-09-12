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
  *          ⭐ 接口 v2(2026-09-11): 语义事件接口 —— 0x200 载荷 =
  *          ev(1B) | arg(2B 大端) | status(1B) | tick(4B 大端)，
  *          0x210 载荷 = status(1B) | uptime_ms(4B 大端) | dev_type | fw_ver | node_id。
  *          总线上**没有** lux/drop% 这类传感器数值：板级只读"语义"，
  *          本节点负责把传感器读数翻译成"车到位/车离开"等事件。
  *
  *          字节级协议表见 c8t6/can.md；本文件只允许改 USER CODE 之间的代码。
  ******************************************************************************
  */
/* USER CODE END Header */
#include "can_node.h"
#include "can.h"        /* hcan(500k, NART=ENABLE, 见 can.c) */
#include "config.h"
#include "gate.h"       /* 道闸执行层: 0x100 指令→Gate_SetTarget, 状态读 Gate_IsOpen */
#include "shade.h"
#include "FreeRTOS.h"
#include "cmsis_os.h"
#include <string.h>

/* freertos.c MX_FREERTOS_Init() 创建的 CAN 发送互斥(USER CODE RTOS_MUTEX 段) */
extern osMutexId_t CanTxMutexHandle;

/* 节点私有状态(单写者规则: s_tx_err 仅 CAN_TxFrame 写; s_last_event_tick 仅 SendEvent 写;
   s_ack_tick/s_gate_latched/s_ready_sent 仅 Poll 写; 其余任务只读, 8/32 位读写天然原子)。
   闸状态不再在 CAN 层维护"逻辑位"——真实到位状态由 gate.c 提供(Gate_IsOpen/IsMoving)。 */
static volatile uint8_t  s_tx_err           = 0u;   /* 1=最近一次发送失败 */
static volatile uint32_t s_last_event_tick  = 0u;   /* 最近成功发 0x200 的 tick */
static volatile uint32_t s_ack_tick         = 0u;   /* 最近收到 0x110 回执的 tick(0=从未) */
static volatile uint32_t s_hb_last_tick     = 0u;   /* 上次心跳 tick */
static uint8_t           s_gate_latched     = 0xFFu;/* 闸位事件基线(0xFF=未初始化) */
static uint8_t           s_ready_sent       = 0u;   /* 上电就绪事件只发一次 */

/* 调试监视(全局开放, 调试器 live watch)。volatile 防止优化器把"只写不读"的全局删掉 */
volatile CAN_NodeDbg_t g_can_node_dbg;

/* 单轮轮询最多处理的收帧数(与 MD文档/can_standard.md §4.4 一致):
   防对端异常洪泛时 CAN_Rx_Task(高优先)长时间占 CPU 饿死低优先任务；
   本端真实负载(心跳 1Hz + 零星指令)远达不到此上限。 */
#define CAN_RX_DRAIN_MAX  16u

/* 汇总 0x200 d[3] / 0x210 d[0] 状态字节(与两帧同源; 语义位, 不含传感器数值) */
static uint8_t StatusBits_Calc(void)
{
  uint8_t b = 0u;
  if (Gate_IsOpen())        b |= CAN_STAT_GATE_OPEN;      /* 执行到位(真实闸位, 非指令态) */
  if (Shade_IsShaded())     b |= CAN_STAT_PRESENCE;       /* 检测区有车/物体 */
  if (Shade_SensorFault())  b |= CAN_STAT_SENSOR_FAULT;
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

/* 组一帧 0x200 语义事件载荷: ev(1B) | arg(2B 大端) | status(1B) | tick(4B 大端) */
static void CAN_BuildEvtPayload(uint8_t *d, uint8_t ev, uint16_t arg, uint32_t tick)
{
  d[0] = ev;
  d[1] = (uint8_t)(arg >> 8);          /* arg 大端: 高字节在前 */
  d[2] = (uint8_t)(arg & 0xFFu);
  d[3] = StatusBits_Calc();
  d[4] = (uint8_t)(tick >> 24);        /* tick 大端: 本节点上电毫秒(u32, 49.7 天回绕) */
  d[5] = (uint8_t)((tick >> 16) & 0xFFu);
  d[6] = (uint8_t)((tick >> 8) & 0xFFu);
  d[7] = (uint8_t)(tick & 0xFFu);
}

/* 组一帧 0x210 心跳载荷: status(1B) | uptime_ms(4B 大端) | dev_type | fw_ver | node_id */
static void CAN_BuildHbPayload(uint8_t *d, uint32_t now)
{
  d[0] = StatusBits_Calc();            /* 与 0x200 d[3] 同源 */
  d[1] = (uint8_t)(now >> 24);
  d[2] = (uint8_t)((now >> 16) & 0xFFu);
  d[3] = (uint8_t)((now >> 8) & 0xFFu);
  d[4] = (uint8_t)(now & 0xFFu);
  d[5] = CAN_DEV_TYPE;                 /* 设备类型(主端据此识别节点种类) */
  d[6] = CAN_FW_VER;                   /* 固件版本 v2.0 */
  d[7] = CAN_NODE_ID;                  /* 节点号(主端据此填 0x21 的 node_id) */
}

/* 0x110 回执由主端(M4)发, 本节点只收(见 can_node.h 的说明) —— 此处不再有发回执的代码。 */

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

  /* 道闸缓动步进(10ms 节拍; 目标由下方 0x100 OPEN/CLOSE case 通过 Gate_SetTarget 设置) */
  Gate_Poll();

  /* ---- 闸位到位边沿 -> 0x200/0x04 GATE_STATE(arg=0 关 / 1 开) ----
     只在"真实到位状态"翻转时发一次(缓动途中不发)，主端/A7 因此无需轮询就能
     拿到闸的真实位置；首拍只建基线(上电时闸在关位，不必上报)。 */
  {
    uint8_t g = Gate_IsOpen() ? 1u : 0u;
    if (s_gate_latched == 0xFFu)
    {
      s_gate_latched = g;
    }
    else if (g != s_gate_latched)
    {
      s_gate_latched = g;
      if (CAN_Node_SendEvent(CAN_EVT_GATE_STATE, (uint16_t)g) == 0u)
      {
        g_can_node_dbg.gate_evts++;
      }
    }
  }

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

      if (HAL_CAN_GetRxMessage(&hcan, CAN_RX_FIFO0, &rh, data) != HAL_OK)
      {
        break;
      }
      drained++;
      g_can_node_dbg.rx_total++;   /* 只要进了 FIFO0 就计(无论是否解析) */

      /* 帧校验(正常模式, can.md 审计整改项): 只收标准帧且 DLC=8——
         0x100 指令 与 0x110 回执(config.h); 其余丢弃。
         硬件掩码过滤器(0x1xx 段)之外的二次软件校验。 */
      if ((rh.IDE != CAN_ID_STD) || (rh.DLC != 8u) ||
          ((rh.StdId != CAN_CMD_ID) && (rh.StdId != CAN_ACK_ID)))
      {
        continue;
      }

      /* 0x110 回执帧按【帧 ID】分发 —— ⚠️ 不能放 switch(data[0]):
         CAN_ACK_ID=0x110(272) 超出 uint8 data[0] 范围永不匹配(曾致回执永不显示);
         且 d[1]=code(事件码)会与 0x100 指令码混淆。
         故在指令解析前单独处理并跳过。 */
      if (rh.StdId == CAN_ACK_ID)
      {
        s_ack_tick = now;          /* 记回执时刻, OLED 行4 显示 Sended 标识 */
        g_can_node_dbg.ack_rx++;
        continue;
      }

      /* 0x100 指令帧: cmd(1B) | arg(1B) | seq(1B) | rsv[5]
         seq 由主端自增, 本节点暂不回执(见 can_node.h 的 0x110 说明):
         指令是否真的生效 -> 0x200/0x04 闸位事件 + 0x210 状态位 bit0。 */
      {
        uint8_t cmd = data[0];
        uint8_t arg = data[1];

        switch (cmd)
        {
          case CAN_CMD_GATE_OPEN:
            g_can_node_dbg.gate_opens++;   /* 收到开闸指令次数 */
            Gate_SetTarget(1u);            /* 缓动开到 90°(到位后才发 0x04 事件) */
            break;

          case CAN_CMD_GATE_CLOSE:
            Gate_SetTarget(0u);            /* 缓动回 0° */
            break;

          case CAN_CMD_STATE_QUERY:
            /* 立即回一帧 0x210(与 1Hz 心跳同格式): 主端刷新整张快照，
               不再用"假事件"当查询应答(那是 v1 的坑: 查询应答被当成边沿)。 */
            {
              uint8_t d[8];
              CAN_BuildHbPayload(d, now);
              (void)CAN_TxFrame(CAN_HB_ID, d);
            }
            break;

          case CAN_CMD_SET_DETECT_LEVEL:
            /* arg = 语义档位 1..5，节点自己翻译成掉点阈值(config.h 表) */
            if (Shade_SetDetectLevel(arg) == 0u)
            {
              g_can_node_dbg.level_sets++;
            }
            break;

          default:              /* 未知指令码: 忽略并计数 */
            g_can_node_dbg.cmd_unknown++;
            break;
        }
      }
    }
  }

  /* ---- 发: 1Hz 心跳(0x210) ---- */
  if ((now >= CAN_HB_PERIOD_MS) && ((now - s_hb_last_tick) >= CAN_HB_PERIOD_MS))
  {
    uint8_t d[8];
    s_hb_last_tick = now;
    CAN_BuildHbPayload(d, now);
    if (CAN_TxFrame(CAN_HB_ID, d) == 0u)
    {
      g_can_node_dbg.hb_sent++;
      if (s_ready_sent == 0u)
      {
        /* 首次心跳成功 = CAN 链路确已工作 => 上报"上电就绪"一次 */
        s_ready_sent = 1u;
        (void)CAN_Node_SendEvent(CAN_EVT_NODE_READY, 0u);
      }
    }
  }
}

uint8_t CAN_Node_SendEvent(uint8_t ev, uint16_t arg)
{
  uint8_t d[8];

  memset(d, 0, sizeof(d));
  CAN_BuildEvtPayload(d, ev, arg, osKernelGetTickCount());
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
  return Gate_IsOpen();   /* 执行到位(非指令态); 缓动中由 OLED 用 Gate_IsMoving 显示 MOVE */
}

uint8_t CAN_Node_TxError(void)
{
  return s_tx_err;
}

uint32_t CAN_Node_LastEventTick(void)
{
  return s_last_event_tick;
}

uint32_t CAN_Node_LastAckTick(void)
{
  return s_ack_tick;
}

uint8_t CAN_Node_StatusBits(void)
{
  return StatusBits_Calc();
}
