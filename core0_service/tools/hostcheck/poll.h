/* hostcheck/poll.h - poll(2) stub for HOST SYNTAX CHECK ONLY.
 * See hostcheck/sys/mman.h for why this exists. */
#ifndef HOSTCHECK_POLL_H
#define HOSTCHECK_POLL_H

#include <sys/types.h>

typedef unsigned long nfds_t;

struct pollfd {
    int   fd;
    short events;
    short revents;
};

#define POLLIN     0x001
#define POLLPRI    0x002
#define POLLOUT    0x004
#define POLLERR    0x008
#define POLLHUP    0x010
#define POLLNVAL   0x020

int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms);

#endif /* HOSTCHECK_POLL_H */
