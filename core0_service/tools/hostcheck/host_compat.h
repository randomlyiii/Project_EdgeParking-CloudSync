/* hostcheck/host_compat.h - force-included on WINDOWS host syntax checks
 * ONLY (Makefile passes -include tools/hostcheck/host_compat.h when the
 * real Linux headers are absent). It fills the small gaps between MinGW
 * and glibc (open flags, pipe/fork/dup2 names) so that the Linux-targeted
 * core0_service sources can be type-checked on a Windows host.
 *
 * Nothing here is linked or shipped to the board.
 */
#ifndef HOSTCHECK_COMPAT_H
#define HOSTCHECK_COMPAT_H

#include <fcntl.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

/* glibc open(2) flags missing from MinGW's <fcntl.h>. Values match Linux
 * so that any code doing arithmetic on them still type-checks the same. */
#ifndef O_NOCTTY
#define O_NOCTTY 0400
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 04000
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 02000000
#endif
#ifndef O_DIRECT
#define O_DIRECT 040000
#endif
#ifndef O_SYNC
#define O_SYNC 04010000
#endif
#ifndef O_DSYNC
#define O_DSYNC 010000
#endif

/* MinGW spells this _pipe(); declare the POSIX name for type checking. */
int pipe(int pipefd[2]);
int fork(void);
int dup2(int oldfd, int newfd);
int fsync(int fd);
int fdatasync(int fd);
unsigned int sleep(unsigned int seconds);
int usleep(unsigned int usec);
char *strdup(const char *s);

#endif /* HOSTCHECK_COMPAT_H */
