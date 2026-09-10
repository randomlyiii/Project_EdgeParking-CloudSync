/* hostcheck/sys/mman.h - POSIX mmap/shm stub for HOST SYNTAX CHECK ONLY.
 *
 * This directory exists so that the core0_service sources (which target
 * Linux) can be compiled on a Windows host with MinGW, where the POSIX
 * headers are absent. It only provides declarations; nothing here is
 * linked into or shipped to the board build. The Makefile adds
 * -Itools/hostcheck ONLY when the real Linux headers are missing
 * (see HAVE_LINUX_HDRS in the Makefile).
 */
#ifndef HOSTCHECK_SYS_MMAN_H
#define HOSTCHECK_SYS_MMAN_H

#include <stddef.h>
#include <sys/types.h>

#define PROT_NONE      0x0
#define PROT_READ      0x1
#define PROT_WRITE     0x2
#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_ANONYMOUS  0x20
#define MAP_ANON       MAP_ANONYMOUS
#define MAP_FAILED     ((void *)-1)
#define MS_ASYNC       1
#define MS_SYNC        4

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int   munmap(void *addr, size_t length);
int   msync(void *addr, size_t length, int flags);
int   mprotect(void *addr, size_t len, int prot);

/* POSIX shared memory objects (glibc: in librt / libc). */
int   shm_open(const char *name, int oflag, mode_t mode);
int   shm_unlink(const char *name);

#endif /* HOSTCHECK_SYS_MMAN_H */
