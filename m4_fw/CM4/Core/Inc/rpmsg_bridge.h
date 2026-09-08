/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    rpmsg_bridge.h
  * @brief   MP157 M4 RPMSG 网关桥：CAN 主端(can_master) ↔ A7-Linux(OpenAMP/VIRT_UART)
  *
  *          职责(PhaseMd/04 P3-02/03/04)：
  *          - 上行: CAN 0x200 事件 → 0x21 帧转发 A7；C8T6 离线/恢复边沿 → 0x22；
  *                  0x13 查询 → 0x23 全量状态；500ms 0x7E 心跳(双向)。
  *          - 下行: 0x11/0x12 开/关闸 → CAN_Master_RequestCmd()(经 CANRxTask 发 0x100)；
  *                  0x13 查询。
  *          - 故障: 1s 查 FDCAN2 HAL_FDCAN_GetProtocolStatus(BusOff) → 自动 Stop/Start
 *                  恢复并计 can_err_cnt。
  *
  *          帧协议与 A7 侧 core0_service/rpmsg 同源(rpmsg_types.h / rpmsg_proto.c 副本)，
  *          权威定义 docs/protocols.md §3。
  *
  *          依赖: CubeMX 勾选 OPENAMP 中间件(生成 CM4/OPENAMP/openamp.{c,h} 等)，
  *          本模块在 freertos.c USER CODE 区创建 Rpmsg_Task(栈 512 words)。
  ******************************************************************************
  */
/* USER CODE END Header */
#ifndef __RPMSG_BRIDGE_H
#define __RPMSG_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/* ---------- 参数 ---------- */
#define RPMSG_BRIDGE_POLL_MS        2u     /* OPENAMP_check_for_message 轮询周期 */
#define RPMSG_BRIDGE_HB_MS          500u   /* 0x7E 心跳周期(协议: 双向 500ms) */
#define RPMSG_BRIDGE_CHK_MS         1000u  /* 状态边沿/Bus_Off 检查周期 */
#define RPMSG_BRIDGE_A7_TIMEOUT_MS  1000u  /* 1s 无有效帧 → A7 链路 DOWN(协议: 断链≤1s) */
#define RPMSG_BRIDGE_TX_RETRIES     3u     /* 发送尝试次数(首发+重试2次, P3-02) */
#define RPMSG_BRIDGE_TX_RETRY_MS    2u     /* 重试间隔 */
#define RPMSG_BRIDGE_EVT_QUEUE_LEN  16u    /* CAN 事件队列深度(满即丢, 事件可丢) */
#define RPMSG_BRIDGE_VUART_RXBUF    512u   /* VIRT_UART 单次 RX 缓冲(协议帧≤489) */
#define RPMSG_BRIDGE_TASK_STACK     512u   /* words; rpmsg_rx_feed 栈帧含 489B 帧 */

/* "rpmsg-tty" 端点创建策略(板端联调 2026-09-10 实测: 首个 NS announce kick 后 Linux 未建
   通道, 现象=mbox IRQ 恒 1 + 无后续流量): 任务启动延时再首次创建, 失败每 2s 自动重试
   (对齐 ST OpenAMP_FreeRTOS_echo 例程语义), 结果写 mon.vuart_init_rc/attempts 可见。 */
#define RPMSG_BRIDGE_VUART_INIT_DELAY_MS  1000u /* 任务启动后首试延时 */
#define RPMSG_BRIDGE_VUART_INIT_RETRY_MS  2000u /* 失败重试间隔 */

/* 0x21 转发过滤: 1=只转发 0x200 事件帧(默认); 0=连 0x210 心跳也转发 */
#define RPMSG_BRIDGE_FWD_HEARTBEAT  0u

/* 运行监视快照(调试器 live watch / 排障; 与 g_can_master_mon 同风格) */
typedef struct {
  uint8_t  vuart_ready;    /* 1=VIRT_UART 初始化+回调注册成功(OPENAMP 起来) */
  uint8_t  vuart_init_rc;  /* 最近一次端点创建结果: 0=OK 1=ERR 0xFF=尚未尝试 */
  uint8_t  a7_alive;       /* 1=1s 内收到过 A7 的 CRC 通过帧 */
  uint8_t  online_latched; /* C8T6 在线状态基线(0xFF=未初始化); 变化即发 0x22 */
  uint32_t vuart_init_attempts; /* 端点创建尝试次数 */
  uint32_t tx_frames;      /* 成功发出帧数(全部 type, 含心跳) */
  uint32_t tx_drop;        /* 重试后仍失败丢弃的帧数 */
  uint32_t rx_frames;      /* 收到 CRC 通过帧数 */
  uint32_t rx_bytes;       /* VIRT_UART 收到的原始字节数 */
  uint32_t evt_fwd;        /* 转发 0x21 的 CAN 帧数 */
  uint32_t evt_drop;       /* 队列满丢弃的 CAN 事件数 */
  uint32_t hb_tx;          /* 0x7E 心跳发出数 */
  uint32_t cmd_rx;         /* 收到的下行指令数(0x11/0x12/0x13) */
  uint32_t unknown_type;   /* 未知 type 帧数(丢弃) */
  uint32_t last_rx_tick;   /* 最近收到有效帧的时刻(ms, osKernelGetTickCount) */
  uint32_t bus_off_cnt;    /* FDCAN2 Bus_Off 恢复次数 */
} RpmsgBridgeMon_t;

extern RpmsgBridgeMon_t g_rpmsg_bridge_mon;

/* 任务入口(freertos.c USER CODE RTOS_THREADS 区创建) */
void Rpmsg_Task(void *argument);

#ifdef __cplusplus
}
#endif

#endif /* __RPMSG_BRIDGE_H */
