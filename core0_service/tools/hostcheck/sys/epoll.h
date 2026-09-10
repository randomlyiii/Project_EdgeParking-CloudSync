/* hostcheck/sys/epoll.h - epoll stub for HOST SYNTAX CHECK ONLY.
 * See hostcheck/sys/mman.h for why this exists. */
#ifndef HOSTCHECK_SYS_EPOLL_H
#define HOSTCHECK_SYS_EPOLL_H

#include <stdint.h>

typedef union epoll_data {
    void    *ptr;
    int      fd;
    uint32_t u32;
    uint64_t u64;
} epoll_data_t;

struct epoll_event {
    uint32_t     events;
    epoll_data_t data;
} __attribute__((packed));

#define EPOLLIN     0x001
#define EPOLLPRI    0x002
#define EPOLLOUT    0x004
#define EPOLLERR    0x008
#define EPOLLHUP    0x010
#define EPOLLRDHUP  0x2000
#define EPOLLET     (1u << 31)

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

#define EPOLL_CLOEXEC 02000000

int epoll_create1(int flags);
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
int epoll_wait(int epfd, struct epoll_event *events, int maxevents,
               int timeout_ms);

#endif /* HOSTCHECK_SYS_EPOLL_H */
