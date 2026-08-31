/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * Shared between the kernel module (ioctl_basics.c) and the userspace
 * test program (ioctl_test.c) - this is the normal shape of a real ioctl
 * interface: one small header, included by both sides, is the entire
 * contract between them.
 */

#ifndef IOCTL_BASICS_H
#define IOCTL_BASICS_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define IOCTL_BASICS_MAGIC 'k'

struct ioctl_basics_stats {
	__u64 reads;
	__u64 echoes;
	__u64 resets;
	__u32 mode;
};

struct ioctl_basics_echo {
	char buf[64];
};

#define IOCTL_BASICS_MODE_IDENTITY 0
#define IOCTL_BASICS_MODE_UPPER 1
#define IOCTL_BASICS_MODE_REVERSE 2

/*
 * The four _IO* macro shapes, one of each:
 *
 *   _IO(type, nr)              no argument payload at all
 *   _IOR(type, nr, datatype)    kernel -> userspace only
 *   _IOW(type, nr, datatype)    userspace -> kernel only
 *   _IOWR(type, nr, datatype)   both directions, same buffer
 *
 * `datatype` is never actually passed to the kernel - these macros only
 * encode sizeof(datatype) into the command number, which is how tools
 * like strace can print a plausible argument size without knowing this
 * driver's semantics.
 */
#define IOCTL_BASICS_RESET _IO(IOCTL_BASICS_MAGIC, 1)
#define IOCTL_BASICS_GET_STATS \
	_IOR(IOCTL_BASICS_MAGIC, 2, struct ioctl_basics_stats)
#define IOCTL_BASICS_SET_MODE _IOW(IOCTL_BASICS_MAGIC, 3, __u32)
#define IOCTL_BASICS_ECHO \
	_IOWR(IOCTL_BASICS_MAGIC, 4, struct ioctl_basics_echo)

#endif /* IOCTL_BASICS_H */
