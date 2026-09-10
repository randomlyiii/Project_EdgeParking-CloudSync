/* hostcheck/termios.h - termios stub for HOST SYNTAX CHECK ONLY.
 * See hostcheck/sys/mman.h for why this exists.
 * Layout mirrors the Linux/glibc termios ABI closely enough for type
 * checking (the real struct comes from the board's libc). */
#ifndef HOSTCHECK_TERMIOS_H
#define HOSTCHECK_TERMIOS_H

#include <sys/types.h>

typedef unsigned char cc_t;
typedef unsigned int  speed_t;
typedef unsigned int  tcflag_t;

#define NCCS 32
struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_line;
    cc_t     c_cc[NCCS];
    speed_t  c_ispeed;
    speed_t  c_ospeed;
};

/* c_cc indices */
#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#define VTIME    5
#define VMIN     6
#define VSTART   8
#define VSTOP    9
#define VSUSP   10

/* c_cflag */
#define CSIZE   0000060
#define CS5     0000000
#define CS6     0000020
#define CS7     0000040
#define CS8     0000060
#define CSTOPB  0000100
#define CREAD   0000200
#define PARENB  0000400
#define CLOCAL  0004000
#define CRTSCTS 020000000000

/* c_iflag */
#define IGNBRK  0000001
#define BRKINT  0000002
#define IGNPAR  0000004
#define INPCK   0000020
#define ISTRIP  0000040
#define INLCR   0000100
#define IGNCR   0000200
#define ICRNL   0000400
#define IXON    0002000

/* c_oflag */
#define OPOST   0000001

/* c_lflag */
#define ISIG    0000001
#define ICANON  0000002
#define ECHO    0000010
#define ECHOE   0000020
#define ECHOK   0000040
#define ECHONL  0000100
#define NOFLSH  0000200
#define IEXTEN  0100000

/* tcsetattr actions */
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

/* baud rates (Linux values) */
#define B0       0000000
#define B9600    0000015
#define B19200   0000016
#define B38400   0000017
#define B57600   0010001
#define B115200  0010002
#define B230400  0010003
#define B460800  0010004
#define B500000  0010005
#define B576000  0010006
#define B921600  0010007
#define B1000000 0010010
#define B1500000 0010012

int  tcgetattr(int fd, struct termios *t);
int  tcsetattr(int fd, int optional_actions, const struct termios *t);
void cfmakeraw(struct termios *t);
int  cfsetispeed(struct termios *t, speed_t speed);
int  cfsetospeed(struct termios *t, speed_t speed);
int  cfsetspeed(struct termios *t, speed_t speed);
void cfsetispeed_raw(struct termios *t, speed_t speed);
int  tcflush(int fd, int queue_selector);
#define TCIFLUSH  0
#define TCOFLUSH  1
#define TCIOFLUSH 2

#endif /* HOSTCHECK_TERMIOS_H */
