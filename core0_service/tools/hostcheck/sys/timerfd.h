/* hostcheck/sys/timerfd.h - timerfd stub for HOST SYNTAX CHECK ONLY.
 * See hostcheck/sys/mman.h for why this exists.
 * NOTE: struct itimerspec is provided by MinGW's <sys/types.h>, so it is
 * NOT redefined here (that caused a redefinition error). */
#ifndef HOSTCHECK_SYS_TIMERFD_H
#define HOSTCHECK_SYS_TIMERFD_H

#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#define TFD_NONBLOCK 04000
#define TFD_CLOEXEC  02000000

#define TFD_TIMER_ABSTIME (1 << 0)
#define TFD_TIMER_CANCEL_ON_SET (1 << 1)

int timerfd_create(int clockid, int flags);
int timerfd_settime(int fd, int flags, const struct itimerspec *new_value,
                    struct itimerspec *old_value);
int timerfd_gettime(int fd, struct itimerspec *curr_value);

#endif /* HOSTCHECK_SYS_TIMERFD_H */
