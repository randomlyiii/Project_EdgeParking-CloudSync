/*
 * rpmsg_demo.c — Linux↔M4 RPMSG 通道演示/联调入口（第3步）
 *
 * 用法：sudo ./rpmsg_demo [-d /dev/ttyRPMSG0]
 *   - 打印链路 UP/DOWN 与所有上行帧（含按协议解码的关键字段）；
 *   - 每 2s 打印一次链路状态 + RX 统计；
 *   - 命令（stdin 回车触发）：
 *       o  发 0x11 开闸        c  发 0x12 关闸
 *       q  发 0x13 查询全量状态  s  立即打印统计
 *       x  退出
 *
 * 对应验收：P3-06 收 M4 心跳；P3-07 长时间解析无误/坏帧被计；P3-08 断链状态；
 *          P3-09 恢复重同步（自动 0x13）；P3-10 事件上行可见。
 * 编译：见 core0_service/Makefile（CROSS_COMPILE 可在板端 SDK 环境置空用 gcc）。
 */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "rpmsg_link.h"
#include "rpmsg_proto.h"

static volatile int g_stop = 0;

static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

static const char *type_name(uint8_t t)
{
    switch (t) {
    case RPMSG_RX_CAN_EVENT:  return "0x21 CAN事件转发";
    case RPMSG_RX_NODE_STATE: return "0x22 从节点离线/恢复";
    case RPMSG_RX_M4_STATE:   return "0x23 M4全量状态";
    case RPMSG_RX_HEARTBEAT:  return "0x7E 心跳";
    default:                  return "未知";
    }
}

static void print_hex(const uint8_t *p, size_t n)
{
    size_t i;
    for (i = 0u; i < n; i++)
        printf("%02X ", (unsigned)p[i]);
}

/* 上行帧解码打印（业务侧消费时可参考此处的字段解析） */
static void on_frame(const rpmsg_frame_t *f, void *opaque)
{
    rpmsg_can_event_t ev;
    rpmsg_m4_state_t  st;
    int nst;

    (void)opaque;
    printf("[RX] type=%s seq=%u len=%u payload=", type_name(f->type),
           (unsigned)f->seq, (unsigned)f->len);
    print_hex(f->payload, f->len);
    printf("\n");

    switch (f->type) {
    case RPMSG_RX_CAN_EVENT:
        if (rpmsg_decode_can_event(f, &ev) == 0) {
            printf("     └ CAN id=0x%03X dlc=%u data=", (unsigned)ev.can_id,
                   (unsigned)ev.dlc);
            print_hex(ev.data, ev.dlc);
            printf(" tick=%u ms\n", (unsigned)ev.tick);
            /* CAN 语义（0x200/0x210）按 docs/protocols.md §1 由业务层解释：
             * 0x200 d[0]=event(0x01 遮光到位) d[1..2]=lux(大端) d[3]=drop% d[4]=状态位 */
            if (ev.can_id == 0x200u && ev.dlc >= 5u) {
                uint16_t lux = (uint16_t)((ev.data[1] << 8) | ev.data[2]);
                printf("     └ 0x200: ev=0x%02X lux=%u drop=%u%%\n",
                       (unsigned)ev.data[0], (unsigned)lux, (unsigned)ev.data[3]);
            }
        }
        break;
    case RPMSG_RX_M4_STATE:
        if (rpmsg_decode_m4_state(f, &st) == 0) {
            printf("     └ gate=%s node=%s can_err=%u\n",
                   st.gate_state ? "开" : "关",
                   st.node_online ? "在线" : "离线", (unsigned)st.can_err_cnt);
        }
        break;
    case RPMSG_RX_NODE_STATE:
        nst = rpmsg_decode_node_state(f);
        printf("     └ 从节点 %s\n", nst == 0 ? "离线" : "恢复在线");
        break;
    default:
        break;
    }
}

static void on_link(rpmsg_link_state_t st, void *opaque)
{
    (void)opaque;
    printf("[LINK] %s @ %ld ms\n", st == RPMSG_LINK_UP ? "UP" : "DOWN",
           (long)(time(NULL)));
}

static void print_stats(const rpmsg_link_t *lk)
{
    rpmsg_rx_stats_t s;

    rpmsg_link_get_stats(lk, &s);
    printf("[STAT] state=%s frames_ok=%llu bad_crc=%llu bad_len=%llu "
           "seq_gaps=%llu resyncs=%llu\n",
           rpmsg_link_get_state(lk) == RPMSG_LINK_UP ? "UP" : "DOWN",
           (unsigned long long)s.frames_ok, (unsigned long long)s.bad_crc,
           (unsigned long long)s.bad_len, (unsigned long long)s.seq_gaps,
           (unsigned long long)s.resyncs);
}

int main(int argc, char *argv[])
{
    const char *dev = "/dev/ttyRPMSG0";
    rpmsg_link_cfg_t cfg;
    rpmsg_link_t *lk;
    time_t last_stat = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            dev = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0) {
            printf("用法: %s [-d <device>]   (默认 %s)\n", argv[0], dev);
            return 0;
        }
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    memset(&cfg, 0, sizeof(cfg));
    cfg.device = dev;
    cfg.on_frame = on_frame;
    cfg.on_link = on_link;
    cfg.hb_interval_ms = 500u;
    cfg.timeout_ms = 1000u;
    cfg.reopen_ms = 1000u;

    lk = rpmsg_link_create(&cfg);
    if (lk == NULL) {
        fprintf(stderr, "rpmsg_link_create 失败\n");
        return 1;
    }

    printf("rpmsg_demo 启动: device=%s\n", dev);
    printf("命令: o=开闸 c=关闸 q=查询 s=统计 x=退出\n");

    if (rpmsg_link_start(lk) != 0) {
        fprintf(stderr, "rpmsg_link_start 失败\n");
        rpmsg_link_free(lk);
        return 1;
    }

    while (!g_stop) {
        struct pollfd pfd;
        time_t now = time(NULL);

        /* 每 2s 打印一次统计（含链路状态） */
        if (now - last_stat >= 2) {
            print_stats(lk);
            last_stat = now;
        }

        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 200) <= 0 || !(pfd.revents & POLLIN))
            continue;   /* 无键盘输入：继续等（链路处理在后台线程） */

        {
            char c;
            ssize_t r;
            int ch;

            do {
                r = read(STDIN_FILENO, &c, 1);
            } while (r < 0 && errno == EINTR);
            if (r <= 0)
                continue;
            ch = (unsigned char)c;
            switch (ch) {
            case 'o':
            case 'O':
                printf("[TX] 开闸 0x11 rc=%d\n", rpmsg_link_send_gate_open(lk));
                break;
            case 'c':
            case 'C':
                printf("[TX] 关闸 0x12 rc=%d\n", rpmsg_link_send_gate_close(lk));
                break;
            case 'q':
            case 'Q':
                printf("[TX] 查询 0x13 rc=%d\n", rpmsg_link_send_query(lk));
                break;
            case 's':
            case 'S':
                print_stats(lk);
                break;
            case 'x':
            case 'X':
                g_stop = 1;
                break;
            default:
                break;
            }
        }
    }

    printf("\n退出中...\n");
    rpmsg_link_free(lk);
    return 0;
}
