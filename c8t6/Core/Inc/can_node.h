/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    can_node.h
  * @brief   C8T6 CAN 从节点模块：本工程 CAN 2.0 帧协议(字节级) + 收发逻辑
  *
  *          协议母本: docs/protocols.md §1(草案母本 PhaseMd/10 CAN 章)
  *          字节级文档: c8t6/can.md(帧格式一节与本文一致，改协议两处同步)
  *
  *          角色: CAN 从节点(bxCAN1 + TJA1050, 500k, 经典 CAN, 标准帧)。
  *          - 收: 主(M4) 0x100 指令(开/关闸、状态查询、灵敏度档位)，轮询 FIFO0(零中断)；
  *          - 发: 0x200 语义事件(车到位/车离开/故障/闸位变化/上电就绪)、
  *                0x210 心跳(1Hz，同时是查询应答)，载荷不含任何传感器数值。
  *          任务归属: main 裸机段 CAN_Node_Init() 初始化；
  *          CAN_Rx_Task 每 CAN_POLL_PERIOD_MS 调 CAN_Node_Poll()；
  *          BH1750_Task 在检测边沿调 CAN_Node_SendEvent()。
  *
  *          ⭐ 接口 v2(2026-09-11): 语义事件接口。板级(A7/M4)只认事件码与
  *          语义参数，不认 lux/drop% 等传感器数值 —— 换传感器不用动 Linux。
  ******************************************************************************
  */
/* USER CODE END Header */
#ifndef __CAN_NODE_H
#define __CAN_NODE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/* ---------- 状态位(0x200 d[3] / 0x210 d[0] 同源；语义化，无传感器数值) ---------- */
#define CAN_STAT_GATE_OPEN    0x01u   /* bit0: 执行器到位(闸已在开位) */
#define CAN_STAT_PRESENCE     0x02u   /* bit1: 检测区当前有车/物体(存在判定) */
#define CAN_STAT_SENSOR_FAULT 0x04u   /* bit2: 传感器子系统故障(读数不可信) */
#define CAN_STAT_CAN_ERR      0x08u   /* bit3: CAN 发送异常(邮箱满/AddTx 失败) */
/* bit4..7 预留 */

/* ---------- 0x100 指令帧(DLC=8: cmd | arg | seq | rsv[5]) ---------- */
#define CAN_CMD_GATE_OPEN       0x01u   /* arg 无 */
#define CAN_CMD_GATE_CLOSE      0x02u   /* arg 无 */
#define CAN_CMD_STATE_QUERY     0x03u   /* 立即回一帧 0x210(不等 1Hz 心跳)；板级当前走 0x13→0x23，此码留作调试/将来用 */
#define CAN_CMD_SET_DETECT_LEVEL 0x10u  /* arg = 灵敏度档位 1..5(语义, 由节点自己解释)；板级下发入口预留 */
/* 0x04 PING 等其余码预留 */

/* ---------- 0x110 回执帧(DLC=8；**只有主端(M4)发、从端只收**) ----------
   kind=0x01: 主端确认收到本节点的一帧 0x200 —— code=事件码, arg=0。
   ⚠️ 本节点**不发** 0x110(它在 0x1xx 主→从段，M4 的硬件过滤器只收 0x200~0x2FF，
   发出去没人能收到)。指令是否生效由 0x200/0x04 闸位事件与 0x210 状态位体现。 */
#define CAN_ACK_KIND_EVENT    0x01u   /* 事件确认(唯一在用值; 其余预留) */

/* ---------- 0x200 事件码(d[0]；与 RPMSG 0x21 用同一张表) ---------- */
#define CAN_EVT_CAR_ARRIVE    0x01u   /* arg=0  车/物体进入检测区 */
#define CAN_EVT_CAR_LEAVE     0x02u   /* arg=0  车/物体离开检测区 */
#define CAN_EVT_NODE_FAULT    0x03u   /* arg=故障类别(见下)，故障"发生"时上报一次 */
#define CAN_EVT_GATE_STATE    0x04u   /* arg=0 关 / 1 开；执行到位边沿上报(2=动作中预留) */
#define CAN_EVT_NODE_READY    0x05u   /* arg=0  上电就绪(首次心跳成功后发一次) */

/* 故障类别(0x200/0x03 的 arg) */
#define CAN_FAULT_SENSOR      1u
#define CAN_FAULT_ACTUATOR    2u
#define CAN_FAULT_CAN         3u

/* 调度器启动前(main USER CODE 2)调用一次：配过滤器(收 0x1xx 段) + Start。
   失败不硬复位：内部记错误，OLED/状态位会体现。 */
void CAN_Node_Init(void);

/* CAN_Rx_Task 每 CAN_POLL_PERIOD_MS 调用：轮询收 FIFO0(指令/查询) + 1Hz 心跳 */
void CAN_Node_Poll(void);

/* 发 0x200 语义事件帧(检测/闸位边沿时调用)。返回 0=成功。
   arg 的含义由事件码决定(见上表)；事件帧同时把"最近事件时刻"记给 OLED 闪烁。 */
uint8_t CAN_Node_SendEvent(uint8_t ev, uint16_t arg);

/* ---------- 供 OLED/状态显示读取 ---------- */
uint8_t  CAN_Node_GateOpen(void);          /* 1=闸执行到位(收 0x100/0x01 后缓动完成, gate.c) */
uint8_t  CAN_Node_TxError(void);           /* 1=最近一次发送失败(状态位 bit3 同源) */
uint32_t CAN_Node_LastEventTick(void);     /* 最近一次成功发 0x200 的 tick(ms)，0=从未 */
uint32_t CAN_Node_LastAckTick(void);       /* 最近一次收到 0x110 回执的 tick(ms)，0=从未 */
uint8_t  CAN_Node_StatusBits(void);        /* 汇总 0x200 d[3]/0x210 d[0] 状态字节 */

/* ---------- 调试监视(供调试器 live watch；类同 M4 g_can_master_mon) ----------
   用于排障：判断"帧到底有没有到、C8T6 能否解码"。CAN_ESR 位定义见 stm32f103xb.h。 */
typedef struct {
  uint32_t rx_total;      /* 收进 FIFO0 的总帧数(无论校验) */
  uint32_t tx_total;      /* 本端成功发出的总帧数(事件+心跳+应答) */
  uint32_t hb_sent;       /* 0x210 心跳发出数 */
  uint32_t ev_sent;       /* 0x200 事件发出数 */
  uint32_t gate_opens;    /* 收到 0x100/0x01 开闸指令的次数 */
  uint32_t gate_evts;     /* 发出的 0x200/0x04 闸位变化事件数 */
  uint32_t ack_rx;        /* 收到的 0x110 事件回执数(主端确认) */
  uint32_t cmd_unknown;   /* 收到的未知 0x100 指令码数(忽略) */
  uint32_t level_sets;    /* 收到的 0x100/0x10 灵敏度档位设置数 */
  uint32_t can_esr_raw;   /* CAN_ESR 寄存器原始值 */
  uint32_t rec;           /* REC = (ESR>>16)&0xFF 接收误差计数(F1 位段, RM0008) */
  uint32_t tec;           /* TEC = (ESR>>8)&0xFF  发送误差计数 */
  uint32_t lec;           /* LEC = (ESR>>4)&0x7   最近错误码 */
  uint32_t boff;          /* 1=当前 BusOff(ESR bit2) */
} CAN_NodeDbg_t;
extern volatile CAN_NodeDbg_t g_can_node_dbg;   /* 全局开放, 调试器/逻辑分析友好 */

#ifdef __cplusplus
}
#endif

#endif /* __CAN_NODE_H */
