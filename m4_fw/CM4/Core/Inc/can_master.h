/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    can_master.h
  * @brief   MP157 M4 CAN 主端(网关)模块：与下位机 C8T6 的 500k 经典 CAN 通讯
  *
  *          协议字节级定义见 c8t6/can.md(与 c8t6 从端共用同一张表)；
  *          角色 = CAN 主节点：发 0x100 指令、收 0x200 事件 / 0x210 心跳。
  *          - 收: CANRxTask 每 CAN_MASTER_POLL_MS 轮询 FDCAN2 RX FIFO0(零中断)；
  *          - 发: 主端单工发送(命令由 Poll 统一提交到 TX FIFO)，无需互斥；
  *          - 在线判定: 主端 3s 收不到 0x210 → 判 C8T6 离线(供后续 RPMSG 上报)。
  *
  *          互动(按键/Linux)说明: 底板按键为 Linux(A7) input 子系统所有，
  *          本模块只暴露 CAN_Master_RequestCmd()/SendCmd() 指令触发口——
  *          先由调试器/后续 RPMSG 驱动；待 Linux+M4(RPMSG) 打通后由
  *          A7·按键 → RPMSG → CAN 0x100 →  C8T6 全链路驱动。
  ******************************************************************************
  */
/* USER CODE END Header */
#ifndef __CAN_MASTER_H
#define __CAN_MASTER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/* CAN 收帧事件钩子: CANRxTask(CAN_Master_Poll) 每解析出一帧标准 8B 帧后回调
   (id, dlc, data, now)。rpmsg_bridge 注册它把 0x200/0x210 上送 A7；
   注册时机: 任意任务早期(单写者, 之后只读)。 */
typedef void (*CAN_Master_EventHook_t)(uint32_t id, uint8_t dlc,
                                       const uint8_t *data, uint32_t now);
void CAN_Master_SetEventHook(CAN_Master_EventHook_t fn);

/* ---------- 帧 ID(与 c8t6/can.md §4、docs/protocols.md §1 一致) ---------- */
#define CAN_MASTER_CMD_ID   0x100u   /* 主→从 指令 */
#define CAN_MASTER_ACK_ID   0x110u   /* 主→从 事件确认: 收到 C8T6 的 0x200 后自动回执,
                                        d[0]=回显 0x200 的 d[0](0x01 遮光/0x00 恢复) */
#define CAN_MASTER_EVT_ID   0x200u   /* 从→主 事件 */
#define CAN_MASTER_HB_ID    0x210u   /* 从→主 心跳 */

/* ---------- 0x100 指令码(d[0]) ---------- */
#define CAN_CMD_OPEN_GATE    0x01u
#define CAN_CMD_CLOSE_GATE   0x02u
#define CAN_CMD_QUERY        0x10u

/* ---------- 状态位(0x200 d[4] / 0x210 d[0] 同源) ---------- */
#define CAN_STAT_GATE_OPEN   0x01u
#define CAN_STAT_SHADED      0x02u
#define CAN_STAT_LUX_FAULT   0x04u
#define CAN_STAT_CAN_ERR     0x08u

/* ---------- 收发/超时参数 ---------- */
#define CAN_MASTER_POLL_MS      10u   /* CANRxTask 轮询周期 ms */
#define CAN_MASTER_OFFLINE_MS 3000u   /* 3s 无 0x210 → C8T6 离线 */

/* ---------- 调试后门(已关闭=正式版, 2026-09-09) ----------
   曾用: >0 时 CAN_Master_Poll 每 N ms 自动发一次开闸指令(0x100/0x01),
   免调试器写变量即可验证 M4→C8T6 方向。设计功能正式化后置 0;
   指令只由 CAN_Master_RequestCmd()/调试器写 g_can_master_cmd_pending 触发
   (第3步起由 RPMSG 驱动)。 */
#define CAN_MASTER_DEBUG_AUTO_GATE_MS 0u

/* 从端状态快照(供调试器 live watch / 后续 RPMSG 上报用) */
typedef struct {
  uint8_t  online;          /* 1=C8T6 在线(收到过 0x210 且未超时) */
  uint8_t  slave_status;    /* 最近状态位(0x210 d[0] / 0x200 d[4]) */
  uint8_t  last_ev;         /* 0x200 d[0]: 0x01 遮光 / 0x00 恢复 */
  uint16_t last_lux;        /* 0x200 d[1..2] 大端 */
  uint8_t  last_drop;       /* 0x200 d[3] drop% */
  uint8_t  last_uptime;     /* 0x210 d[1] 上电秒低8位 */
  uint32_t last_hb_tick;    /* 最近一次收 0x210 的时刻(ms) */
  uint32_t last_ev_tick;    /* 最近一次收 0x200 的时刻(ms) */
  uint32_t hb_count;        /* 收到心跳次数 */
  uint32_t ev_count;        /* 收到事件次数 */
  uint32_t rx_total;        /* 收进 RX FIFO0 的总帧数(无论是否解析; 回环自测看这个) */
  uint32_t tx_ok_count;     /* 指令发送成功次数(入队) */
  uint32_t tx_err_count;    /* 指令发送失败次数 */
  uint32_t tx_fifo_free;    /* TX FIFO 空闲元素数(0~8). 发完被ACK会回满; 一直低=没人ACK */
  uint32_t tx_fifo_fill;    /* TX FIFO 中未完成(未ACK)帧数 = 8 - free */
} CAN_MasterMonitor_t;

/* 调度器启动前(main USER CODE 2)调用一次: 标准掩码过滤(收 0x200~0x2FF)
   + 全局过滤 + Start。失败不硬复位, 记入状态。 */
void CAN_Master_Init(void);

/* CANRxTask 每 CAN_MASTER_POLL_MS 调用: 提交待发指令 + 轮询收 FIFO0 + 离线判定 */
void CAN_Master_Poll(void);

/* 立即发送一帧 0x100 指令(任务上下文)。返回 0=已入 TX FIFO, 1=失败 */
uint8_t CAN_Master_SendCmd(uint8_t cmd);

/* 收到 0x200 后自动回一帧 0x110 事件确认(任务上下文)。返回 0=已入 TX FIFO, 1=失败 */
uint8_t CAN_Master_SendAck(uint8_t ev);

/* 置"待发指令"标志(单次), 由下一次 CAN_Master_Poll 统一发送。
   供调试器 live watch / 后续 RPMSG 任务使用, 无需直接占用 CAN。 */
void CAN_Master_RequestCmd(uint8_t cmd);

/* 拷出当前监视快照到缓冲区 */
void CAN_Master_GetMonitor(CAN_MasterMonitor_t *m);

/* 供调试器直接查看的待发指令(写这些全局变量亦等效于 RequestCmd) */
extern volatile uint8_t g_can_master_cmd_pending;
extern volatile uint8_t g_can_master_cmd_value;

/* 从端状态快照(全局开放, 调试器 live watch 直接看; 亦可用 GetMonitor 拷贝) */
extern CAN_MasterMonitor_t g_can_master_mon;

#ifdef __cplusplus
}
#endif

#endif /* __CAN_MASTER_H */
