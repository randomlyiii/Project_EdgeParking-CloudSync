/* hostcheck/sys/eventfd.h - eventfd stub for HOST SYNTAX CHECK ONLY.
 * See hostcheck/sys/mman.h for why this exists. */
#ifndef HOSTCHECK_SYS_EVENTFD_H
#define HOSTCHECK_SYS_EVENTFD_H

#include <stdint.h>

typedef uint64_t eventfd_t;

#define EFD_SEMAPHORE 1
#define EFD_CLOEXEC   02000000
#define EFD_NONBLOCK  04000

int eventfd(unsigned int initval, int flags);
int eventfd_read(int fd, eventfd_t *value);
int eventfd_write(int fd, eventfd_t value);

#endif /* HOSTCHECK_SYS_EVENTFD_H */
