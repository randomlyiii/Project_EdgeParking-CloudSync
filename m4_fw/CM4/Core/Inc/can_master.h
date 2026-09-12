/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    can_master.h
  * @brief   MP157 M4 CAN 主端(网关)模块：与下位机 C8T6 的 500k 经典 CAN 通讯
  *
  *          协议字节级定义见 c8t6/can.md(与 c8t6 从端共用同一张表)；
  *          角色 = CAN 主节点：发 0x100 指令 / 0x110 回执，收 0x200 事件 / 0x210 心跳。
  *          - 收: CANRxTask 每 CAN_MASTER_POLL_MS 轮询 FDCAN2 RX FIFO0(零中断)；
  *          - 发: 主端单工发送(命令由 Poll 统一提交到 TX FIFO)，无需互斥；
  *          - 在线判定: 主端 3s 收不到 0x210 → 判 C8T6 离线(供后续 RPMSG 上报)。
  *
  *          ⭐ 接口 v2(2026-09-11): 设备抽象 / 传感器隔离 —— 本模块只认
  *          "事件码 + 语义参数 + 状态位 + 节点身份"，**不保存也不转发 lux/drop%**
  *          这类传感器数值(板级与下位机解耦，换传感器不动 Linux)。
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
#define CAN_MASTER_ACK_ID   0x110u   /* 主→从 回执: kind(0x01 事件确认)|code(事件码)|0 */
#define CAN_MASTER_EVT_ID   0x200u   /* 从→主 事件 */
#define CAN_MASTER_HB_ID    0x210u   /* 从→主 心跳(1Hz; 也是查询应答) */

/* ---------- 0x100 指令码(d[0]) ---------- */
#define CAN_CMD_GATE_OPEN        0x01u
#define CAN_CMD_GATE_CLOSE       0x02u
#define CAN_CMD_STATE_QUERY      0x03u   /* 从端立即回一帧 0x210 */
#define CAN_CMD_SET_DETECT_LEVEL 0x10u   /* d[1]=语义档位 1..5 */

/* ---------- 0x110 回执(kind=0x01 事件确认；本端发、从端收) ---------- */
#define CAN_ACK_KIND_EVENT   0x01u   /* d[1]=被确认的 0x200 事件码, d[2]=0 */
/* 0x02 及其余 kind 预留(从端不产生 0x100 指令回执: 指令效果由 0x200/0x04 闸位事件体现) */

/* ---------- 状态位(0x200 d[3] / 0x210 d[0] 同源; 语义位) ---------- */
#define CAN_STAT_GATE_OPEN    0x01u   /* bit0: 执行器到位(闸开) */
#define CAN_STAT_PRESENCE     0x02u   /* bit1: 检测区有车/物体 */
#define CAN_STAT_SENSOR_FAULT 0x04u   /* bit2: 传感器子系统故障 */
#define CAN_STAT_CAN_ERR      0x08u   /* bit3: CAN 发送异常 */

/* ---------- 收发/超时参数 ---------- */
#define CAN_MASTER_POLL_MS      10u   /* CANRxTask 轮询周期 ms */
#define CAN_MASTER_OFFLINE_MS 3000u   /* 3s 无 0x210 → C8T6 离线 */

/* ---------- 调试后门(已关闭=正式版, 2026-09-09; 09-10 曾临时置 3000 验证 C8T6 收 open, 已置回) ----------
   曾用: >0 时 CAN_Master_Poll 每 N ms 自动发一次开闸指令(0x100/0x01),
   免调试器写变量即可验证 M4→C8T6 方向。设计功能正式化后置 0;
   指令只由 CAN_Master_RequestCmd()/调试器写 g_can_master_cmd_pending 触发
   (第3步起由 RPMSG 驱动)。2026-09-10 板验 C8T6 收 open/close 经 RPMSG 全链路通过。 */
#define CAN_MASTER_DEBUG_AUTO_GATE_MS 0u

/* 从端状态快照(供调试器 live watch / 后续 RPMSG 上报用)。
   ⚠️ 只放语义量: 任何传感器原始值(光照/距离/掉点百分比…)都不进这一层。 */
typedef struct {
  uint8_t  online;              /* 1=C8T6 在线(收到过 0x210 且未超时) */
  uint8_t  slave_status;        /* 最近状态位(0x210 d[0] / 0x200 d[3]) */
  uint8_t  last_ev;             /* 0x200 d[0] 事件码 */
  uint16_t last_arg;            /* 0x200 d[1..2] 语义参数(大端解析后) */
  uint32_t last_ev_tick;        /* 最近一次收 0x200 的时刻(ms) */
  uint32_t last_uptime_ms;      /* 0x210 d[1..4] 节点上电毫秒(大端) */
  uint8_t  dev_type;            /* 0x210 d[5] 设备类型 */
  uint8_t  fw_ver;              /* 0x210 d[6] 固件版本(高4位主/低4位次) */
  uint8_t  node_id;             /* 0x210 d[7] 节点号(0=未知, 未收到心跳时) */
  uint32_t last_hb_tick;        /* 最近一次收 0x210 的时刻(ms) */
  uint32_t hb_count;            /* 收到心跳次数 */
  uint32_t ev_count;            /* 收到事件次数 */
  uint32_t rx_total;            /* 收进 RX FIFO0 的总帧数(无论是否解析; 回环自测看这个) */
  uint32_t tx_ok_count;         /* 指令/回执发送成功次数(入队) */
  uint32_t tx_err_count;        /* 发送失败次数 */
  uint32_t tx_fifo_free;        /* TX FIFO 空闲元素数(0~8). 发完被ACK会回满; 一直低=没人ACK */
  uint32_t tx_fifo_fill;        /* TX FIFO 中未完成(未ACK)帧数 = 8 - free */
} CAN_MasterMonitor_t;

/* 调度器启动前(main USER CODE 2)调用一次: 标准掩码过滤(收 0x200~0x2FF)
   + 全局过滤 + Start。失败不硬复位, 记入状态。 */
void CAN_Master_Init(void);

/* CANRxTask 每 CAN_MASTER_POLL_MS 调用: 提交待发指令 + 轮询收 FIFO0 + 离线判定 */
void CAN_Master_Poll(void);

/* 立即发送一帧 0x100 指令(任务上下文)。cmd 见 CAN_CMD_*; arg 语义由 cmd 决定
   (如 SET_DETECT_LEVEL 的档位)；seq 由内部自增并等回执回显。
   返回 0=已入 TX FIFO, 1=失败 */
uint8_t CAN_Master_SendCmd(uint8_t cmd, uint8_t arg);

/* 回一帧 0x110 回执(收到 0x200 事件后自动调用; 任务上下文)。
   kind 见 CAN_ACK_KIND_*。返回 0=已入 TX FIFO, 1=失败 */
uint8_t CAN_Master_SendAck(uint8_t kind, uint8_t code, uint8_t arg);

/* 置"待发指令"标志(单次), 由下一次 CAN_Master_Poll 统一发送。
   供调试器 live watch / RPMSG 任务使用, 无需直接占用 CAN。 */
void CAN_Master_RequestCmd(uint8_t cmd, uint8_t arg);

/* 拷出当前监视快照到缓冲区 */
void CAN_Master_GetMonitor(CAN_MasterMonitor_t *m);

/* 供调试器直接查看的待发指令(写这些全局变量亦等效于 RequestCmd) */
extern volatile uint8_t g_can_master_cmd_pending;
extern volatile uint8_t g_can_master_cmd_value;
extern volatile uint8_t g_can_master_cmd_arg;

/* 从端状态快照(全局开放, 调试器 live watch 直接看; 亦可用 GetMonitor 拷贝) */
extern CAN_MasterMonitor_t g_can_master_mon;

#ifdef __cplusplus
}
#endif

#endif /* __CAN_MASTER_H */
