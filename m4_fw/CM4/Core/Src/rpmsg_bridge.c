/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    rpmsg_bridge.c
  * @brief   CAN 主端(can_master) ↔ A7-Linux(OpenAMP/VIRT_UART) 网关桥实现
  *
  *          线程模型(全部 OpenAMP 交互收敛在本任务, OpenAMP 非线程安全):
  *          - CANRxTask: CAN_Master_Poll 收帧 → s_evt_hook → 事件队列(深 16, 满丢);
  *            pending 指令(g_can_master_cmd_pending)由 Poll 统一提交 CAN TX。
  *          - Rpmsg_Task(本文件): OPENAMP_check_for_message 轮询 RX(回调仅 memcpy+置
  *            标志, 回调跑在 check_for_message 调用者上下文, 禁调 FreeRTOS API——
  *            ST OpenAMP_FreeRTOS_echo 同款约束) → 拆帧分发 → 心跳/事件/状态发送。
  *
  *          对齐协议 docs/protocols.md §3(= core0_service/rpmsg):
  *          - 0x11/0x12 → CAN_Master_RequestCmd(0x01/0x02), 0x13 → 0x23, 0x7E 保鲜;
  *          - 0x21 = id u32 LE + dlc + data[8] 原样 + tick u32 LE(osKernelGetTickCount);
  *          - 0x23 = gate(slave_status&bit0) + online + can_err_cnt u16 LE(饱和,
  *            = bus_off_cnt + mon.tx_err_count);
  *          - seq 按 type 独立计数(u16 自然回绕); 发送失败重试 2 次后丢弃计数。
  *
  *          前置: m4_fw.ioc 勾选 OPENAMP 中间件并 Regenerate(生成 CM4/OPENAMP/ 与
  *          MX_IPCC_Init/MX_OPENAMP_Init), 见 m4_fw/rpmsg.md。本文件不碰生成代码。
  ******************************************************************************
  */
/* USER CODE END Header */
#include "main.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include <string.h>
#include "fdcan.h"            /* hfdcan2: Bus_Off 检测用 HAL_FDCAN_GetProtocolStatus */
#include "can_master.h"
#include "rpmsg_bridge.h"

#if defined(__GNUC__) && !defined(RPMSG_BRIDGE_ALLOW_NO_OPENAMP)
#if !__has_include("openamp.h")
#error "rpmsg_bridge.c: openamp.h not found -- CubeMX 未勾选 OPENAMP 中间件或未 Regenerate (m4_fw.ioc -> Middleware/OPENAMP, 步骤见 m4_fw/rpmsg.md P3-01)"
#endif
#endif

#include "openamp.h"          /* CubeMX 生成(CM4/OPENAMP/) -> openamp_conf.h */
#include "virt_uart.h"        /* VIRT_UART_* API(Middlewares/Third_Party/OpenAMP/virtual_driver),
                                 显式包含——生成 openamp.h 不保证带出 */
#include "ipcc.h"             /* MX_IPCC_Init(CubeMX 生成, IPCC 外设) */

#include "rpmsg_types.h"
#include "rpmsg_proto.h"

/* ============================ 运行状态(静态分配, 不占任务栈) ============================ */

RpmsgBridgeMon_t g_rpmsg_bridge_mon;

static VIRT_UART_HandleTypeDef s_vuart;                       /* "rpmsg-tty" 端点 -> /dev/ttyRPMSG0 */
static rpmsg_rx_t      s_rx;                                  /* 拆帧状态机(~2.9KB, 静态) */
static uint8_t         s_vuart_rxbuf[RPMSG_BRIDGE_VUART_RXBUF];
static volatile uint16_t s_vuart_rxlen;
static volatile uint8_t  s_vuart_rxflag;
static uint8_t         s_txbuf[RPMSG_FRAME_MAX];              /* 组帧发送缓冲(489B) */
static uint16_t        s_txseq[256];                          /* 发送 seq: 按 type 独立计数 */
static osMessageQueueId_t s_evt_q;                            /* CAN 事件队列(生产者 CANRxTask) */
static uint8_t         s_hb_ord;                              /* 0x7E 序号 0..255 */
static volatile uint8_t s_query_pending;                      /* 0x13 → 0x23 待发 */
static uint32_t        s_last_hb_tick;
static uint32_t        s_last_chk_tick;
static uint32_t        s_last_init_tick;                      /* "rpmsg-tty" 端点创建节拍 */

/* CAN 事件队列元素(hook 在 CANRxTask 上下文填充) */
typedef struct {
  uint32_t id;
  uint8_t  dlc;
  uint8_t  data[8];                                           /* 未用字节 0 填充 */
  uint32_t tick;
} rpmsg_can_evt_msg_t;

/* ============================ 小工具 ============================ */

static void wr_u16_le(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)(v >> 8);
}

static void wr_u32_le(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
  p[2] = (uint8_t)((v >> 16) & 0xFFu);
  p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* 发送一帧: 组帧(per-type seq) + VIRT_UART_Transmit 重试(首发+2 次) */
static uint8_t bridge_send(uint8_t type, const uint8_t *payload, uint16_t plen)
{
  int n;
  uint8_t attempt;

  if (g_rpmsg_bridge_mon.vuart_ready == 0u)
    return 1u;

  n = rpmsg_frame_build(type, s_txseq[type], payload, plen,
                        s_txbuf, sizeof(s_txbuf));
  if (n <= 0)
    return 1u;
  s_txseq[type] = (uint16_t)(s_txseq[type] + 1u);             /* 回绕自然处理 */

  for (attempt = 0u; attempt < RPMSG_BRIDGE_TX_RETRIES; attempt++)
  {
    if (VIRT_UART_Transmit(&s_vuart, s_txbuf, (uint16_t)n) == VIRT_UART_OK)
    {
      g_rpmsg_bridge_mon.tx_frames++;
      return 0u;
    }
    osDelay(RPMSG_BRIDGE_TX_RETRY_MS);                        /* 等 A7 归还 vring buffer */
  }
  g_rpmsg_bridge_mon.tx_drop++;                               /* 事件类可丢, 状态靠 0x13 重同步 */
  return 1u;
}

/* 0x23 M4 全量状态: gate + node_online + can_err_cnt u16 LE(饱和) */
static void bridge_send_m4_state(void)
{
  CAN_MasterMonitor_t mm;
  uint8_t p[RPMSG_M4_STATE_LEN];
  uint32_t err;

  CAN_Master_GetMonitor(&mm);
  p[0] = (mm.slave_status & CAN_STAT_GATE_OPEN) ? 1u : 0u;
  p[1] = mm.online;
  err = (uint32_t)g_rpmsg_bridge_mon.bus_off_cnt + mm.tx_err_count;
  if (err > 0xFFFFu)
    err = 0xFFFFu;
  wr_u16_le(p + 2, (uint16_t)err);
  (void)bridge_send(RPMSG_RX_M4_STATE, p, RPMSG_M4_STATE_LEN);
}

/* 0x21 CAN 事件转发: 17B, data 原样搬运(lux 大端不转换) */
static void bridge_send_can_event(const rpmsg_can_evt_msg_t *m)
{
  uint8_t p[RPMSG_CAN_EVT_LEN];

  wr_u32_le(p, m->id);
  p[4] = m->dlc;
  memcpy(p + 5, m->data, 8);
  wr_u32_le(p + 13, m->tick);
  (void)bridge_send(RPMSG_RX_CAN_EVENT, p, RPMSG_CAN_EVT_LEN);
}

/* ============================ VIRT_UART RX 回调 ============================ */
/* 跑在 OPENAMP_check_for_message 调用者(Rpmsg_Task)上下文: 只允许 memcpy/置标志,
   禁调任何 FreeRTOS API(OpenAMP 非线程安全, ST 例程同款约束)。 */
static void vuart_rx_cb(VIRT_UART_HandleTypeDef *huart)
{
  uint16_t n = (huart->RxXferSize < RPMSG_BRIDGE_VUART_RXBUF)
               ? huart->RxXferSize : (RPMSG_BRIDGE_VUART_RXBUF - 1u);
  memcpy(s_vuart_rxbuf, huart->pRxBuffPtr, n);
  s_vuart_rxlen  = n;
  s_vuart_rxflag = 1u;
}

/* ============================ 拆帧分发(rpmsg_rx_feed 回调) ============================ */
/* 任何 CRC 通过的帧都保鲜 a7_alive(协议: 0x7E/0x21 均可)。 */
static int frame_cb(const rpmsg_frame_t *f, void *opaque)
{
  (void)opaque;
  g_rpmsg_bridge_mon.rx_frames++;
  g_rpmsg_bridge_mon.last_rx_tick = osKernelGetTickCount();
  g_rpmsg_bridge_mon.a7_alive = 1u;

  switch (f->type)
  {
    case RPMSG_TX_GATE_OPEN:                                  /* 0x11 → CAN 0x100/0x01 */
      g_rpmsg_bridge_mon.cmd_rx++;
      CAN_Master_RequestCmd(CAN_CMD_OPEN_GATE);
      break;
    case RPMSG_TX_GATE_CLOSE:                                 /* 0x12 → CAN 0x100/0x02 */
      g_rpmsg_bridge_mon.cmd_rx++;
      CAN_Master_RequestCmd(CAN_CMD_CLOSE_GATE);
      break;
    case RPMSG_TX_QUERY_STATE:                                /* 0x13 → 回 0x23(循环里发) */
      g_rpmsg_bridge_mon.cmd_rx++;
      s_query_pending = 1u;
      break;
    case RPMSG_RX_HEARTBEAT:                                  /* 0x7E: 仅保鲜 */
      break;
    default:
      g_rpmsg_bridge_mon.unknown_type++;
      break;
  }
  return 0;                                                   /* 继续拆帧 */
}

/* ============================ CAN 事件钩子(CANRxTask 上下文) ============================ */
static void can_evt_hook(uint32_t id, uint8_t dlc, const uint8_t *data, uint32_t tick)
{
  rpmsg_can_evt_msg_t m;

  if (s_evt_q == NULL)
    return;
  memset(&m, 0, sizeof(m));
  m.id = id;
  m.dlc = (dlc > 8u) ? 8u : dlc;
  m.tick = tick;
  if (data != NULL)
    memcpy(m.data, data, m.dlc);
  if (osMessageQueuePut(s_evt_q, &m, 0U, 0U) != osOK)         /* 满即丢(事件可丢) */
    g_rpmsg_bridge_mon.evt_drop++;
}

/* ============================ 1s 维护: 0x22 边沿 + Bus_Off 恢复 ============================ */
static void periodic_check(uint32_t now)
{
  uint8_t online = g_can_master_mon.online;
  FDCAN_ProtocolStatusTypeDef pst;

  /* C8T6 离线/恢复边沿 → 0x22(首个检查点只同步基线, 不发) */
  if (g_rpmsg_bridge_mon.online_latched == 0xFFu)
  {
    g_rpmsg_bridge_mon.online_latched = online;
  }
  else if (online != g_rpmsg_bridge_mon.online_latched)
  {
    uint8_t v = (online != 0u) ? 1u : 0u;
    g_rpmsg_bridge_mon.online_latched = online;
    (void)bridge_send(RPMSG_RX_NODE_STATE, &v, RPMSG_NODE_STATE_LEN);
  }

  /* P3-04: Bus_Off 检测(HAL API; FDCAN 无 ABOM, 软件恢复) → Stop/Start + 0x23 上报 */
  if ((HAL_FDCAN_GetProtocolStatus(&hfdcan2, &pst) == HAL_OK) && (pst.BusOff != 0u))
  {
    (void)HAL_FDCAN_Stop(&hfdcan2);
    (void)HAL_FDCAN_Start(&hfdcan2);
    g_rpmsg_bridge_mon.bus_off_cnt++;
    g_can_master_mon.tx_err_count++;                          /* 并入 can_err_cnt 口径 */
    s_query_pending = 1u;                                     /* 恢复后经 0x23 告知 A7 */
  }

  (void)now;
}

/* ============================ 任务入口 ============================ */
void Rpmsg_Task(void *argument)
{
  (void)argument;

  memset(&g_rpmsg_bridge_mon, 0, sizeof(g_rpmsg_bridge_mon));
  g_rpmsg_bridge_mon.online_latched = 0xFFu;                  /* 未初始化基线 */
  g_rpmsg_bridge_mon.vuart_init_rc  = 0xFFu;                  /* 端点尚未尝试创建 */

  /* ⚠️ 2026-09-10 板验修正: OpenAMP 框架初始化(原 main.c 调度器前)移入本任务。
     原因: MX_OPENAMP_Init 内 wait_remote_ready 会无限忙等(Linux 未就绪场景),
     放 main 会卡死整个系统(FDCAN/CAN/调度器全起不来, CAN 全聋)。
     在此初始化: 卡死只冻结本任务, CANRxTask/defaultTask 照常跑。
     工程模式(CubeIDE 单跑)无 Linux 主端, 跳过。 */
  if (!IS_ENGINEERING_BOOT_MODE())
  {
    MX_IPCC_Init();
    (void)MX_OPENAMP_Init(RPMSG_REMOTE, NULL);
  }

  s_evt_q = osMessageQueueNew(RPMSG_BRIDGE_EVT_QUEUE_LEN,
                              sizeof(rpmsg_can_evt_msg_t), NULL);
  rpmsg_rx_init(&s_rx);
  CAN_Master_SetEventHook(can_evt_hook);

  /* "rpmsg-tty" 端点: OPENAMP 框架已由 main.c 的 MX_IPCC_Init + MX_OPENAMP_Init 就绪,
     这里延时首试+失败自动重试(头文件宏), 成功后 A7 侧出现 /dev/ttyRPMSG0。
     失败不挂死: 结果见 mon.vuart_ready/vuart_init_rc/attempts, CAN 业务不受影响。 */
  s_last_init_tick = 0u;
  s_last_hb_tick = osKernelGetTickCount();
  s_last_chk_tick = s_last_hb_tick;

  for (;;)
  {
    uint32_t now = osKernelGetTickCount();

    /* ---- "rpmsg-tty" 端点创建(延时 + 失败重试) ---- */
    if ((g_rpmsg_bridge_mon.vuart_ready == 0u) &&
        (now >= RPMSG_BRIDGE_VUART_INIT_DELAY_MS) &&
        ((now - s_last_init_tick) >= RPMSG_BRIDGE_VUART_INIT_RETRY_MS))
    {
      VIRT_UART_StatusTypeDef rc;
      uint8_t r;
      s_last_init_tick = now;
      g_rpmsg_bridge_mon.vuart_init_attempts++;
      rc = VIRT_UART_Init(&s_vuart);
      r = (rc == VIRT_UART_OK) ? 0u : (uint8_t)rc;
      if ((rc == VIRT_UART_OK) &&
          (VIRT_UART_RegisterCallback(&s_vuart, VIRT_UART_RXCPLT_CB_ID,
                                      vuart_rx_cb) != VIRT_UART_OK))
      {
        r = (uint8_t)VIRT_UART_ERROR;                         /* Init OK 但回调注册失败 */
      }
      g_rpmsg_bridge_mon.vuart_init_rc = r;
      if (r == 0u)
      {
        g_rpmsg_bridge_mon.vuart_ready = 1u;
      }
    }

    /* ---- RX: OpenAMP 轮询(回调 memcpy 到静态缓冲) + 拆帧分发 ---- */
    OPENAMP_check_for_message();
    if (s_vuart_rxflag != 0u)
    {
      uint16_t n = s_vuart_rxlen;
      s_vuart_rxflag = 0u;
      g_rpmsg_bridge_mon.rx_bytes += n;
      if (g_rpmsg_bridge_mon.a7_alive == 0u)
      {
        rpmsg_rx_reset(&s_rx);                                /* A7 重启/重连: 作废旧 seq 基线 */
      }
      (void)rpmsg_rx_feed(&s_rx, s_vuart_rxbuf, n, frame_cb, NULL);
    }
    else if ((g_rpmsg_bridge_mon.a7_alive != 0u) &&
             ((now - g_rpmsg_bridge_mon.last_rx_tick) > RPMSG_BRIDGE_A7_TIMEOUT_MS))
    {
      g_rpmsg_bridge_mon.a7_alive = 0u;                       /* 1s 无有效帧 → 链路 DOWN(只标记) */
    }

    /* ---- 0x13 查询应答(0x23) ---- */
    if (s_query_pending != 0u)
    {
      s_query_pending = 0u;
      bridge_send_m4_state();
    }

    /* ---- 500ms 双向心跳 0x7E(A7 未连也照发, rpmsg_tty 侧自丢) ---- */
    if ((now - s_last_hb_tick) >= RPMSG_BRIDGE_HB_MS)
    {
      uint8_t ord;
      s_last_hb_tick = now;
      ord = s_hb_ord++;
      g_rpmsg_bridge_mon.hb_tx++;
      (void)bridge_send(RPMSG_RX_HEARTBEAT, &ord, RPMSG_HB_LEN);
    }

    /* ---- CAN 事件队列 → 0x21 转发 ---- */
    if (s_evt_q != NULL)
    {
      rpmsg_can_evt_msg_t m;
      while (osMessageQueueGet(s_evt_q, &m, NULL, 0U) == osOK)
      {
#if (RPMSG_BRIDGE_FWD_HEARTBEAT == 0u)
        if (m.id != CAN_MASTER_EVT_ID)                        /* 默认只转发 0x200 事件 */
          continue;
#endif
        bridge_send_can_event(&m);
        g_rpmsg_bridge_mon.evt_fwd++;
      }
    }

    /* ---- 1s 维护: 0x22 边沿 + Bus_Off ---- */
    if ((now - s_last_chk_tick) >= RPMSG_BRIDGE_CHK_MS)
    {
      s_last_chk_tick = now;
      periodic_check(now);
    }

    osDelay(RPMSG_BRIDGE_POLL_MS);
  }
}
