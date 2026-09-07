/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    can_node.h
  * @brief   C8T6 CAN 从节点模块：本工程 CAN 2.0 帧协议(字节级) + 收发逻辑
  *
  *          协议母本: PhaseMd/10_协议规格总表(CAN 章) / PhaseMd/03 第2步
  *          字节级文档: c8t6/can.md(帧格式一节与本文一致，改协议两处同步)
  *
  *          角色: CAN 从节点(bxCAN1 + TJA1050, 500k, 经典 CAN, 标准帧)。
  *          - 收: 主(M4) 0x100 指令(开/关闸、查询)，轮询 FIFO0(零中断)；
  *          - 发: 0x200 遮光事件(遮光/恢复边沿、查询应答)、0x210 心跳(1Hz)。
  *          任务归属: main 裸机段 CAN_Node_Init() 初始化；
  *          CAN_Rx_Task 每 CAN_POLL_PERIOD_MS 调 CAN_Node_Poll()；
  *          BH1750_Task 在遮光边沿调 CAN_Node_SendEvent()。
  ******************************************************************************
  */
/* USER CODE END Header */
#ifndef __CAN_NODE_H
#define __CAN_NODE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/* ---------- 0x200/0x210 状态位(两帧同源, 见 can.md §帧格式) ---------- */
#define CAN_STAT_GATE_OPEN   0x01u   /* bit0: 闸处于开位 */
#define CAN_STAT_SHADED      0x02u   /* bit1: 遮光中(车到位) */
#define CAN_STAT_LUX_FAULT   0x04u   /* bit2: 光感(BH1750)故障 */
#define CAN_STAT_CAN_ERR     0x08u   /* bit3: CAN 发送异常(邮箱满/AddTx 失败) */

/* ---------- 0x100 指令码(d[0]) ---------- */
#define CAN_CMD_OPEN_GATE    0x01u   /* 开闸 */
#define CAN_CMD_CLOSE_GATE   0x02u   /* 关闸 */
#define CAN_CMD_QUERY        0x10u   /* 查询: 从端回一帧 0x200 快照 */

/* ---------- 0x200 事件码(d[0]) ---------- */
#define CAN_EVT_RECOVER      0x00u   /* 恢复(解除遮光) */
#define CAN_EVT_SHADED       0x01u   /* 遮光/车到位 */

/* 调度器启动前(main USER CODE 2)调用一次：配过滤器(收 0x1xx 段) + Start。
   失败不硬复位：内部记错误，OLED/状态位会体现。 */
void CAN_Node_Init(void);

/* CAN_Rx_Task 每 CAN_POLL_PERIOD_MS 调用：轮询收 FIFO0(指令/查询) + 1Hz 心跳 */
void CAN_Node_Poll(void);

/* 发 0x200 事件帧(遮光/恢复边沿时由 BH1750_Task 调)。返回 0=成功。
   事件帧同时把"最近事件时刻"记录给 OLED 做 CAN:EVT 闪烁。 */
uint8_t CAN_Node_SendEvent(uint8_t ev, uint16_t lux, uint8_t drop);

/* ---------- 供 OLED/状态显示读取 ---------- */
uint8_t  CAN_Node_GateOpen(void);          /* 1=闸开(收到 0x100/0x01) */
uint8_t  CAN_Node_TxError(void);           /* 1=最近一次发送失败(状态位 bit3 同源) */
uint32_t CAN_Node_LastEventTick(void);     /* 最近一次成功发 0x200 的 tick(ms)，0=从未 */
uint8_t  CAN_Node_StatusBits(void);        /* 汇总 0x200 d[4]/0x210 d[0] 状态字节 */

/* ---------- 调试监视(供调试器 live watch；类同 M4 g_can_master_mon) ----------
   用于排障：判断"帧到底有没有到、C8T6 能否解码"。CAN_ESR 位定义见 stm32f103xb.h。 */
typedef struct {
  uint32_t rx_total;      /* 收进 FIFO0 的总帧数(无论校验) */
  uint32_t tx_total;      /* 本端成功发出的总帧数(事件+心跳+应答) */
  uint32_t hb_sent;       /* 0x210 心跳发出数 */
  uint32_t ev_sent;       /* 0x200 事件发出数 */
  uint32_t gate_opens;    /* 收到 0x100/0x01 开闸指令的次数(Gate 行翻转来源) */
  uint32_t can_esr_raw;   /* CAN_ESR 寄存器原始值 */
  uint32_t rec;           /* = (ESR>>24)&0xFF 接收误差计数 */
  uint32_t tec;           /* = (ESR>>16)&0xFF 发送误差计数 */
  uint32_t lec;           /* = (ESR>>4)&0x7   最近错误码 */
  uint32_t boff;          /* 1=当前 BusOff */
} CAN_NodeDbg_t;
extern volatile CAN_NodeDbg_t g_can_node_dbg;   /* 全局开放, 调试器/逻辑分析友好 */

#ifdef __cplusplus
}
#endif

#endif /* __CAN_NODE_H */
