#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "ioctl_basics.h"

static void print_stats(const struct ioctl_basics_stats *s)
{
	printf("  reads=%llu echoes=%llu resets=%llu mode=%u\n",
	       (unsigned long long)s->reads, (unsigned long long)s->echoes,
	       (unsigned long long)s->resets, s->mode);
}

static int do_echo(int fd, const char *text)
{
	struct ioctl_basics_echo req;

	memset(&req, 0, sizeof(req));
	strncpy(req.buf, text, sizeof(req.buf) - 1);

	if (ioctl(fd, IOCTL_BASICS_ECHO, &req) < 0) {
		perror("ioctl(ECHO)");
		return -1;
	}

	printf("  \"%s\" -> \"%s\"\n", text, req.buf);

	return 0;
}

int main(int argc, char **argv)
{
	struct ioctl_basics_stats stats;
	__u32 mode;
	int fd;

	if (argc != 2) {
		fprintf(stderr, "usage: %s /dev/ioctl_basicsX\n", argv[0]);
		return EXIT_FAILURE;
	}

	fd = open(argv[1], O_RDWR);
	if (fd < 0) {
		perror("open");
		return EXIT_FAILURE;
	}

	printf("=== RESET ===\n");
	if (ioctl(fd, IOCTL_BASICS_RESET) < 0) {
		perror("ioctl(RESET)");
		return EXIT_FAILURE;
	}

	printf("=== GET_STATS (after reset) ===\n");
	if (ioctl(fd, IOCTL_BASICS_GET_STATS, &stats) < 0) {
		perror("ioctl(GET_STATS)");
		return EXIT_FAILURE;
	}
	print_stats(&stats);

	printf("=== ECHO, identity mode ===\n");
	do_echo(fd, "Hello, kernel!");

	printf("=== SET_MODE upper ===\n");
	mode = IOCTL_BASICS_MODE_UPPER;
	if (ioctl(fd, IOCTL_BASICS_SET_MODE, &mode) < 0) {
		perror("ioctl(SET_MODE)");
		return EXIT_FAILURE;
	}
	do_echo(fd, "Hello, kernel!");

	printf("=== SET_MODE reverse ===\n");
	mode = IOCTL_BASICS_MODE_REVERSE;
	ioctl(fd, IOCTL_BASICS_SET_MODE, &mode);
	do_echo(fd, "Hello, kernel!");

	printf("=== SET_MODE invalid value (expect EINVAL) ===\n");
	mode = 99;
	if (ioctl(fd, IOCTL_BASICS_SET_MODE, &mode) < 0)
		printf("  failed as expected: %s\n", strerror(errno));
	else
		printf("  unexpectedly succeeded\n");

	printf("=== unknown command (expect ENOTTY) ===\n");
	if (ioctl(fd, _IO('k', 99)) < 0)
		printf("  failed as expected: %s\n", strerror(errno));
	else
		printf("  unexpectedly succeeded\n");

	printf("=== GET_STATS (final) ===\n");
	if (ioctl(fd, IOCTL_BASICS_GET_STATS, &stats) < 0) {
		perror("ioctl(GET_STATS)");
		return EXIT_FAILURE;
	}
	print_stats(&stats);

	close(fd);

	return EXIT_SUCCESS;
}
