/*
 * rpmsg_proto.c — RPMSG 帧编解码层实现 [M4 副本]
 *
 * CRC16 = XMODEM（poly 0x1021 / init 0x0000 / 不反射 / 输出无异或）
 * 校验范围 type..payload（不含帧头）；帧尾 2B 低字节在前。
 * 自检向量：crc16("123456789") == 0x31C3。
 *
 * ⭐ 本文件 = core0_service/rpmsg/rpmsg_proto.c 的完整拷贝副本（M4 侧
 *    rpmsg_decode_* 为死代码但保留，保证两端逐字节可 diff；单一事实源在
 *    A7 侧，改动需走协议同步流程：母本 §3 → A7 → 本副本 → 两处变更记录）。
 *    纯 C（stdint/stddef/string），无 OS/Linux 依赖，M4 工程直接编译。
 */
#include "rpmsg_proto.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* CRC16-XMODEM                                                        */
/* ------------------------------------------------------------------ */
uint16_t rpmsg_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0x0000;
    size_t i;

    for (i = 0; i < len; i++) {
        unsigned bit;
        crc ^= (uint16_t)data[i] << 8;
        for (bit = 0; bit < 8; bit++) {
            if (crc & 0x8000u)
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            else
                crc = (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* ------------------------------------------------------------------ */
/* 组帧                                                                */
/* ------------------------------------------------------------------ */
int rpmsg_frame_build(uint8_t type, uint16_t seq,
                      const uint8_t *payload, uint16_t plen,
                      uint8_t *out, size_t outcap)
{
    uint16_t crc;
    size_t total;

    if (plen > RPMSG_PAYLOAD_MAX)
        return -1;
    total = 2u + 1u + 2u + 2u + plen + 2u; /* 帧头+type+seq+len+payload+crc */
    if (out == NULL || outcap < total)
        return -1;

    out[0] = RPMSG_MAGIC0;
    out[1] = RPMSG_MAGIC1;
    out[2] = type;
    out[3] = (uint8_t)(seq & 0xFFu);
    out[4] = (uint8_t)(seq >> 8);
    out[5] = (uint8_t)(plen & 0xFFu);
    out[6] = (uint8_t)(plen >> 8);
    if (plen > 0u && payload != NULL)
        memcpy(out + 7, payload, plen);

    /* CRC 校验范围 type..payload */
    crc = rpmsg_crc16(out + 2, 1u + 2u + 2u + plen);
    out[7u + plen]     = (uint8_t)(crc & 0xFFu);
    out[8u + plen]     = (uint8_t)(crc >> 8);
    return (int)total;
}

/* ------------------------------------------------------------------ */
/* 拆帧状态机                                                          */
/* ------------------------------------------------------------------ */
void rpmsg_rx_init(rpmsg_rx_t *rx)
{
    if (rx == NULL)
        return;
    memset(rx, 0, sizeof(*rx));
}

void rpmsg_rx_reset(rpmsg_rx_t *rx)
{
    if (rx == NULL)
        return;
    rx->len = 0u;
    /* 清空 per-type seq 基线：重连后首帧即新基准，旧 seq 不再计入缺口 */
    memset(rx->stats.last_seq, 0, sizeof(rx->stats.last_seq));
    memset(rx->stats.seen, 0, sizeof(rx->stats.seen));
}

static size_t rx_scan_magic(const uint8_t *p, size_t n)
{
    size_t i;
    for (i = 0; i + 1 < n; i++) {
        if (p[i] == RPMSG_MAGIC0 && p[i + 1] == RPMSG_MAGIC1)
            return i;
    }
    return (size_t)-1; /* 未找到 */
}

static int rx_track_seq(rpmsg_rx_t *rx, uint8_t type, uint16_t seq)
{
    if (rx->stats.seen[type]) {
        uint16_t last = rx->stats.last_seq[type];
        if (seq != last) {
            /* 正常相邻：差 1；跳变=丢帧缺口（65535→0 回绕由模运算自然处理） */
            uint16_t gap = (uint16_t)(uint16_t)(seq - last) - 1u;
            rx->stats.seq_gaps += gap;
        }
        rx->stats.last_seq[type] = seq;
        return 0;
    }
    rx->stats.seen[type] = 1u;
    rx->stats.last_seq[type] = seq;
    return 0;
}

size_t rpmsg_rx_feed(rpmsg_rx_t *rx, const uint8_t *data, size_t n,
                     rpmsg_frame_cb cb, void *opaque)
{
    size_t decoded = 0u;

    if (rx == NULL || data == NULL)
        return 0u;
    if (n == 0u)
        return 0u;

    /* 追加到累积缓冲（空间不足时丢弃最旧字节腾出空间） */
    if (rx->len + n > sizeof(rx->buf)) {
        size_t excess = rx->len + n - sizeof(rx->buf);
        size_t drop = (excess < rx->len) ? excess : rx->len;
        memmove(rx->buf, rx->buf + drop, rx->len - drop);
        rx->len -= drop;
        rx->stats.resyncs++;
    }
    memcpy(rx->buf + rx->len, data, n);
    rx->len += n;

    for (;;) {
        size_t magic_at;
        uint16_t plen, crc_stored, crc_calc;
        uint8_t type;
        uint16_t seq;
        rpmsg_frame_t f;
        size_t frame_len;

        /* 1) 找帧头 */
        magic_at = rx_scan_magic(rx->buf, rx->len);
        if (magic_at == (size_t)-1) {
            /* 整段无帧头：仅当末字节可能是 AA 前缀时保留 1 字节，其余丢弃 */
            if (rx->len == 1u && rx->buf[0] != RPMSG_MAGIC0) {
                rx->len = 0u;               /* 单字节且非 AA：无保留价值 */
            } else if (rx->len > 1u) {
                rx->buf[0] = rx->buf[rx->len - 1u];
                rx->len = 1u;
                rx->stats.resyncs++;
            }
            break;
        }
        if (magic_at > 0u) {
            /* 帧头前有垃圾：丢弃，计一次重同步 */
            memmove(rx->buf, rx->buf + magic_at, rx->len - magic_at);
            rx->len -= magic_at;
            rx->stats.resyncs++;
        }

        /* 2) 头部解析需要 2+1+2+2 = 7 字节 */
        if (rx->len < 7u)
            break;
        type = rx->buf[2];
        seq  = (uint16_t)(rx->buf[3] | ((uint16_t)rx->buf[4] << 8));
        plen = (uint16_t)(rx->buf[5] | ((uint16_t)rx->buf[6] << 8));

        /* 3) 长度合法性：len 超限 → 坏帧，丢掉帧头继续重同步 */
        if (plen > RPMSG_PAYLOAD_MAX) {
            rx->stats.bad_len++;
            rx->stats.resyncs++;
            memmove(rx->buf, rx->buf + 1u, rx->len - 1u);
            rx->len -= 1u;
            continue;
        }

        frame_len = 2u + 1u + 2u + 2u + plen + 2u;
        if (rx->len < frame_len)
            break; /* 帧未收全，等更多数据 */

        /* 4) CRC 校验（type..payload） */
        crc_stored = (uint16_t)(rx->buf[2u + 1u + 2u + 2u + plen]
                                | ((uint16_t)rx->buf[3u + 1u + 2u + 2u + plen] << 8));
        crc_calc = rpmsg_crc16(rx->buf + 2, 1u + 2u + 2u + plen);

        if (crc_stored != crc_calc) {
            rx->stats.bad_crc++;
            rx->stats.resyncs++;
            /* 丢 1 字节（帧头 AA）继续搜，防坏帧内嵌新帧头 */
            memmove(rx->buf, rx->buf + 1u, rx->len - 1u);
            rx->len -= 1u;
            continue;
        }

        /* 5) 有效帧交付：先回调后消费，回调要求停止时帧保留在缓冲待下轮 */
        {
            int stop = 0;

            f.type = type;
            f.seq = seq;
            f.len = plen;
            if (plen > 0u)
                memcpy(f.payload, rx->buf + 7, plen);

            if (cb != NULL && cb(&f, opaque) != 0)
                stop = 1;

            if (!stop) {
                rx->stats.frames_ok++;
                rx_track_seq(rx, type, seq);
                memmove(rx->buf, rx->buf + frame_len, rx->len - frame_len);
                rx->len -= frame_len;
                decoded++;
            }
            if (stop)
                break; /* 缓冲满等：帧留在缓冲，等下次喂入 */
        }
    }
    return decoded;
}

/* ------------------------------------------------------------------ */
/* 单帧便捷解析                                                        */
/* ------------------------------------------------------------------ */
int rpmsg_rx_parse_one(const uint8_t *frame, size_t n, rpmsg_frame_t *out)
{
    uint16_t plen, crc_stored, crc_calc;
    size_t frame_len;

    if (frame == NULL || out == NULL || n < 7u)
        return -1;
    if (frame[0] != RPMSG_MAGIC0 || frame[1] != RPMSG_MAGIC1)
        return -1;

    plen = (uint16_t)(frame[5] | ((uint16_t)frame[6] << 8));
    if (plen > RPMSG_PAYLOAD_MAX)
        return -1;
    frame_len = 2u + 1u + 2u + 2u + plen + 2u;
    if (n < frame_len)
        return -1;

    crc_stored = (uint16_t)(frame[frame_len - 2u] | ((uint16_t)frame[frame_len - 1u] << 8));
    crc_calc = rpmsg_crc16(frame + 2, 1u + 2u + 2u + plen);
    if (crc_stored != crc_calc)
        return -1;

    out->type = frame[2];
    out->seq  = (uint16_t)(frame[3] | ((uint16_t)frame[4] << 8));
    out->len  = plen;
    if (plen > 0u)
        memcpy(out->payload, frame + 7, plen);
    return (int)frame_len;
}

/* ------------------------------------------------------------------ */
/* 上行 payload 解码                                                    */
/* ------------------------------------------------------------------ */
static uint16_t rd_u16_le(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

int rpmsg_decode_node_event(const rpmsg_frame_t *f, rpmsg_node_event_t *ev)
{
    const uint8_t *p;

    if (f == NULL || ev == NULL)
        return -1;
    if (f->type != RPMSG_RX_NODE_EVENT || f->len < RPMSG_NODE_EVT_LEN)
        return -1;

    p = f->payload;
    ev->code    = p[0];
    ev->arg     = rd_u16_le(p + 1);
    ev->status  = p[3];
    ev->node_id = p[4];
    ev->tick    = rd_u32_le(p + 5);
    return 0;
}

int rpmsg_decode_m4_state(const rpmsg_frame_t *f, rpmsg_m4_state_t *st)
{
    const uint8_t *p;

    if (f == NULL || st == NULL)
        return -1;
    if (f->type != RPMSG_RX_M4_STATE || f->len < RPMSG_M4_STATE_LEN)
        return -1;

    p = f->payload;
    st->gate_state  = p[0];
    st->node_online = p[1];
    st->can_err_cnt = rd_u16_le(p + 2);
    return 0;
}

int rpmsg_decode_node_state(const rpmsg_frame_t *f)
{
    if (f == NULL)
        return -1;
    if (f->type != RPMSG_RX_NODE_STATE || f->len < RPMSG_NODE_STATE_LEN)
        return -1;
    return (f->payload[0] == 0u) ? 0 : 1;
}
