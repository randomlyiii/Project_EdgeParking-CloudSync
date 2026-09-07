/*
 * rpmsg_cli.c — RPMSG 打桩 CLI（PhaseMd/04 P3-11）
 *
 * 用途：不依赖业务代码，直接对 /dev/ttyRPMSG0 做 hex 收发/坏帧注入/丢帧统计，
 *       独立验证 M4 侧行为（也可以对 M4 模拟发 0x11/0x12/0x13）。
 *
 * 用法：sudo ./rpmsg_cli [-d <device>]     默认 /dev/ttyRPMSG0
 * 命令：
 *   tx <type-hex> [payload hex...]   发一帧（seq 按 type 自动递增）
 *   raw <hex...>                     原样发一串字节（任意注入）
 *   bad <type-hex> [payload hex...]  发“CRC 故意改坏”的帧（测对端容错）
 *   stats                            打印接收统计
 *   help / quit
 *
 * 接收：所有读到的字节先做 hex 回显，再尝试按协议拆帧打印。
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "rpmsg_proto.h"

static volatile int g_stop = 0;

static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void cfg_raw(int fd)
{
    struct termios tio;
    if (!isatty(fd))
        return;
    if (tcgetattr(fd, &tio) != 0)
        return;
    cfmakeraw(&tio);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSANOW, &tio);
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* "AA 55 11 ..." / "aa55..." → 二进制 */
static int parse_hex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    int hi = -1;

    for (; *s; s++) {
        if (*s == ' ' || *s == '\t' || *s == ',' || *s == ':')
            continue;
        if (*s == '\n' || *s == '\r')
            break;
        {
            int v = hex_val(*s);
            if (v < 0)
                return -1;
            if (hi < 0)
                hi = v;
            else {
                if (n >= cap)
                    return -2;
                out[n++] = (uint8_t)((hi << 4) | v);
                hi = -1;
            }
        }
    }
    if (hi >= 0)
        return -1;              /* 奇数个 nibble */
    return (int)n;
}

static int write_all(int fd, const uint8_t *buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, buf + off, n - off);
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

/* 接收侧解析统计 */
static rpmsg_rx_t g_rx;

static void print_rx_bytes(const uint8_t *b, size_t n)
{
    size_t i;
    printf("<< ");
    for (i = 0; i < n; i++)
        printf("%02X ", b[i]);
    printf("\n");
}

static void print_stats(void)
{
    printf("stats: ok=%llu bad_crc=%llu bad_len=%llu seq_gaps=%llu resyncs=%llu\n",
           (unsigned long long)g_rx.stats.frames_ok,
           (unsigned long long)g_rx.stats.bad_crc,
           (unsigned long long)g_rx.stats.bad_len,
           (unsigned long long)g_rx.stats.seq_gaps,
           (unsigned long long)g_rx.stats.resyncs);
}

static void usage(void)
{
    printf("命令: tx <type-hex> [payload...] | raw <hex...> | "
           "bad <type-hex> [payload...] | stats | help | quit\n");
}

int main(int argc, char *argv[])
{
    const char *dev = "/dev/ttyRPMSG0";
    int fd;
    int i;
    uint16_t tx_seq[256];
    struct pollfd pfds[2];
    char line[1024];

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            dev = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0) {
            printf("用法: %s [-d <device>]\n", argv[0]);
            return 0;
        }
    }

    memset(tx_seq, 0, sizeof(tx_seq));
    rpmsg_rx_init(&g_rx);

    fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
        return 1;
    }
    cfg_raw(fd);

    signal(SIGINT, on_sigint);
    printf("rpmsg_cli: %s (quit 退出)\n", dev);
    usage();

    pfds[0].fd = fd;
    pfds[0].events = POLLIN;
    pfds[1].fd = STDIN_FILENO;
    pfds[1].events = POLLIN;

    while (!g_stop) {
        pfds[0].revents = 0;
        pfds[1].revents = 0;
        if (poll(pfds, 2, 500) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (pfds[0].revents & (POLLIN | POLLERR | POLLHUP)) {
            uint8_t tmp[512];
            ssize_t r;
            for (;;) {
                r = read(fd, tmp, sizeof(tmp));
                if (r > 0) {
                    print_rx_bytes(tmp, (size_t)r);
                    rpmsg_rx_feed(&g_rx, tmp, (size_t)r, NULL, NULL);
                } else if (r == 0) {
                    printf("<< EOF\n");
                    g_stop = 1;
                    break;
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                } else if (errno == EINTR) {
                    continue;
                } else {
                    printf("read err: %s\n", strerror(errno));
                    g_stop = 1;
                    break;
                }
            }
        }

        if (pfds[1].revents & POLLIN) {
            if (fgets(line, sizeof(line), stdin) == NULL)
                break;
            /* 去掉尾部换行 */
            {
                size_t L = strlen(line);
                while (L > 0 && (line[L - 1] == '\n' || line[L - 1] == '\r'))
                    line[--L] = '\0';
            }

            if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
                usage();
            } else if (strcmp(line, "quit") == 0 || strcmp(line, "q") == 0) {
                g_stop = 1;
            } else if (strcmp(line, "stats") == 0) {
                print_stats();
            } else if (strncmp(line, "tx ", 3) == 0) {
                uint8_t buf[RPMSG_FRAME_MAX];
                uint8_t payload[RPMSG_PAYLOAD_MAX];
                const char *s = line + 3;
                char type_s[8];
                int plen;
                int type;

                while (*s == ' ') s++;
                {
                    size_t k = 0;
                    while (s[k] && s[k] != ' ' && k < sizeof(type_s) - 1)
                        type_s[k++] = s[k];
                    type_s[k] = '\0';
                }
                type = (int)strtol(type_s, NULL, 16);
                {
                    const char *p = strchr(s, ' ');
                    if (p != NULL)
                        p++; else p = s + strlen(s);
                    plen = parse_hex(p, payload, sizeof(payload));
                }
                if (plen < 0) {
                    printf("payload hex 解析失败\n");
                    continue;
                }
                {
                    int total = rpmsg_frame_build((uint8_t)type, tx_seq[type & 0xFF]++,
                                                  payload, (uint16_t)plen,
                                                  buf, sizeof(buf));
                    if (total < 0) {
                        printf("组帧失败（payload 过长？）\n");
                        continue;
                    }
                    printf(">> type=0x%02X seq=%u len=%u\n", (unsigned)(type & 0xFF),
                           (unsigned)(tx_seq[type & 0xFF] - 1u), (unsigned)plen);
                    if (write_all(fd, buf, (size_t)total) != 0)
                        printf("写失败: %s\n", strerror(errno));
                }
            } else if (strncmp(line, "raw ", 4) == 0) {
                uint8_t buf[512];
                int n = parse_hex(line + 4, buf, sizeof(buf));
                if (n < 0) {
                    printf("hex 解析失败\n");
                    continue;
                }
                printf(">> raw %d 字节\n", n);
                if (write_all(fd, buf, (size_t)n) != 0)
                    printf("写失败: %s\n", strerror(errno));
            } else if (strncmp(line, "bad ", 4) == 0) {
                /* 同 tx，但 CRC 字节故意改坏 → 测对端丢帧容错 */
                uint8_t buf[RPMSG_FRAME_MAX];
                uint8_t payload[RPMSG_PAYLOAD_MAX];
                char type_s[8];
                const char *s = line + 4;
                int type;
                int plen;

                while (*s == ' ') s++;
                {
                    size_t k = 0;
                    while (s[k] && s[k] != ' ' && k < sizeof(type_s) - 1)
                        type_s[k++] = s[k];
                    type_s[k] = '\0';
                }
                type = (int)strtol(type_s, NULL, 16);
                {
                    const char *p = strchr(s, ' ');
                    if (p != NULL)
                        p++; else p = s + strlen(s);
                    plen = parse_hex(p, payload, sizeof(payload));
                }
                if (plen < 0) {
                    printf("payload hex 解析失败\n");
                    continue;
                }
                {
                    int total = rpmsg_frame_build((uint8_t)type, tx_seq[type & 0xFF]++,
                                                  payload, (uint16_t)plen,
                                                  buf, sizeof(buf));
                    if (total < 0) {
                        printf("组帧失败\n");
                        continue;
                    }
                    buf[total - 1] ^= 0xFFu;   /* 破坏 CRC 高字节 */
                    printf(">> bad-crc type=0x%02X（CRC 故意改坏）\n",
                           (unsigned)(type & 0xFF));
                    if (write_all(fd, buf, (size_t)total) != 0)
                        printf("写失败: %s\n", strerror(errno));
                }
            } else if (line[0] != '\0') {
                usage();
            }
        }
    }

    close(fd);
    print_stats();
    return 0;
}
