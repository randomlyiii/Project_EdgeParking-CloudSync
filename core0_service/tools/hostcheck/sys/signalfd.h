/* hostcheck/sys/signalfd.h - signalfd stub for HOST SYNTAX CHECK ONLY.
 * See hostcheck/sys/mman.h for why this exists.
 * MinGW's <signal.h> has no sigset_t / sigprocmask, so the small POSIX
 * signal-set surface used by core0_main.c is declared here too. */
#ifndef HOSTCHECK_SYS_SIGNALFD_H
#define HOSTCHECK_SYS_SIGNALFD_H

#include <stdint.h>
#include <signal.h>

#ifndef HOSTCHECK_SIGSET_T_DEFINED
#define HOSTCHECK_SIGSET_T_DEFINED
typedef struct { unsigned long __bits[16]; } sigset_t;
#endif

struct signalfd_siginfo {
    uint32_t ssi_signo;
    int32_t  ssi_errno;
    int32_t  ssi_code;
    uint32_t ssi_pid;
    uint32_t ssi_uid;
    int32_t  ssi_fd;
    uint32_t ssi_tid;
    uint32_t ssi_band;
    uint32_t ssi_overrun;
    uint32_t ssi_trapno;
    int32_t  ssi_status;
    int32_t  ssi_int;
    uint64_t ssi_ptr;
    uint64_t ssi_utime;
    uint64_t ssi_stime;
    uint64_t ssi_addr;
    uint8_t  __pad[48];
};
#define SIGINFO_SIZE 128

#define SFD_NONBLOCK 04000
#define SFD_CLOEXEC  02000000

int signalfd(int fd, const sigset_t *mask, int flags);

/* POSIX signal-set helpers (absent from MinGW). */
int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int signum);
int sigdelset(sigset_t *set, int signum);
int sigismember(const sigset_t *set, int signum);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);

#ifndef SIG_BLOCK
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#endif

#endif /* HOSTCHECK_SYS_SIGNALFD_H */
