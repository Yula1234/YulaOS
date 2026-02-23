// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#ifndef YOS_IOCTL_H
#define YOS_IOCTL_H

#include <stdint.h>

#define _YOS_IOC_NRBITS    8u
#define _YOS_IOC_TYPEBITS  8u
#define _YOS_IOC_SIZEBITS  14u
#define _YOS_IOC_DIRBITS   2u

#define _YOS_IOC_NRSHIFT   0u
#define _YOS_IOC_TYPESHIFT (_YOS_IOC_NRSHIFT + _YOS_IOC_NRBITS)
#define _YOS_IOC_SIZESHIFT (_YOS_IOC_TYPESHIFT + _YOS_IOC_TYPEBITS)
#define _YOS_IOC_DIRSHIFT  (_YOS_IOC_SIZESHIFT + _YOS_IOC_SIZEBITS)

#define _YOS_IOC_NONE  0u
#define _YOS_IOC_WRITE 1u
#define _YOS_IOC_READ  2u

#define _YOS_IOC(dir, type, nr, size) \
    (((uint32_t)(dir)  << _YOS_IOC_DIRSHIFT)  | \
     ((uint32_t)(type) << _YOS_IOC_TYPESHIFT) | \
     ((uint32_t)(nr)   << _YOS_IOC_NRSHIFT)   | \
     ((uint32_t)(size) << _YOS_IOC_SIZESHIFT))

#define _YOS_IO(type, nr)           _YOS_IOC(_YOS_IOC_NONE,  (type), (nr), 0u)
#define _YOS_IOR(type, nr, data_t)  _YOS_IOC(_YOS_IOC_READ,  (type), (nr), (uint32_t)sizeof(data_t))
#define _YOS_IOW(type, nr, data_t)  _YOS_IOC(_YOS_IOC_WRITE, (type), (nr), (uint32_t)sizeof(data_t))
#define _YOS_IOWR(type, nr, data_t) _YOS_IOC(_YOS_IOC_READ | _YOS_IOC_WRITE, (type), (nr), (uint32_t)sizeof(data_t))

#define _YOS_IOC_DIR(req)  (((uint32_t)(req) >> _YOS_IOC_DIRSHIFT) & ((1u << _YOS_IOC_DIRBITS) - 1u))
#define _YOS_IOC_TYPE(req) (((uint32_t)(req) >> _YOS_IOC_TYPESHIFT) & ((1u << _YOS_IOC_TYPEBITS) - 1u))
#define _YOS_IOC_NR(req)   (((uint32_t)(req) >> _YOS_IOC_NRSHIFT) & ((1u << _YOS_IOC_NRBITS) - 1u))
#define _YOS_IOC_SIZE(req) (((uint32_t)(req) >> _YOS_IOC_SIZESHIFT) & ((1u << _YOS_IOC_SIZEBITS) - 1u))

typedef uint32_t yos_tcflag_t;
typedef uint8_t  yos_cc_t;

typedef struct {
    yos_tcflag_t c_iflag;
    yos_tcflag_t c_oflag;
    yos_tcflag_t c_cflag;
    yos_tcflag_t c_lflag;
    yos_cc_t     c_line;
    yos_cc_t     c_cc[32];
} yos_termios_t;

enum {
    YOS_VINTR = 0,
    YOS_VQUIT = 1,
    YOS_VSUSP = 2,
    YOS_VMIN  = 16,
    YOS_VTIME = 17,
};

enum {
    YOS_IFLAG_IGNCR = 1u << 0,
    YOS_IFLAG_ICRNL = 1u << 1,
    YOS_IFLAG_INLCR = 1u << 2,

    YOS_IFLAG_IXON  = 1u << 3,
    YOS_IFLAG_IXOFF = 1u << 4,
};

enum {
    YOS_OFLAG_OPOST = 1u << 0,
    YOS_OFLAG_ONLCR = 1u << 1,
};

enum {
    YOS_LFLAG_ISIG = 1u << 0,
    YOS_LFLAG_ICANON = 1u << 1,
    YOS_LFLAG_ECHO = 1u << 2,
    YOS_LFLAG_TOSTOP = 1u << 3,
};

typedef struct {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
} yos_winsize_t;

typedef struct {
    int32_t delta;
} yos_tty_scroll_t;

#define YOS_TCGETS     _YOS_IOR('T', 0x19, yos_termios_t)
#define YOS_TCSETS     _YOS_IOW('T', 0x1A, yos_termios_t)
#define YOS_TIOCGWINSZ _YOS_IOR('T', 0x13, yos_winsize_t)
#define YOS_TIOCSWINSZ _YOS_IOW('T', 0x14, yos_winsize_t)
#define YOS_TIOCGPTN   _YOS_IOR('T', 0x15, uint32_t)
#define YOS_TTY_SCROLL _YOS_IOW('T', 0x16, yos_tty_scroll_t)

#define YOS_TIOCSCTTY  _YOS_IO('T', 0x17)
#define YOS_TCGETPGRP  _YOS_IOR('T', 0x18, uint32_t)
#define YOS_TCSETPGRP  _YOS_IOW('T', 0x1B, uint32_t)

#define YOS_TIOCGPGRP  YOS_TCGETPGRP
#define YOS_TIOCSPGRP  YOS_TCSETPGRP

#define YOS_TIOCGSID   _YOS_IOR('T', 0x1C, uint32_t)

typedef struct {
    uint8_t mac[6];
} yos_net_mac_t;

#define YOS_NET_GET_MAC _YOS_IOR('N', 0x01, yos_net_mac_t)

#endif
