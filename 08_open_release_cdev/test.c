#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int run_open_test(const char *path, int flags, const char *name)
{
	int fd;

	printf("\n=== %s ===\n", name);
	printf("userspace open flags: 0x%x\n", flags);

	fd = open(path, flags);
	if (fd < 0) {
		perror("open");
		return -1;
	}

	printf("opened fd=%d\n", fd);

	if (close(fd) < 0) {
		perror("close");
		return -1;
	}

	printf("closed fd=%d\n", fd);

	return 0;
}

static int run_dup_test(const char *path)
{
	int fd;
	int duplicate;

	printf("\n=== DUPLICATED FILE DESCRIPTOR TEST ===\n");

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror("open");
		return -1;
	}

	duplicate = dup(fd);
	if (duplicate < 0) {
		perror("dup");
		close(fd);
		return -1;
	}

	printf("original fd=%d, duplicate fd=%d\n", fd, duplicate);
	printf("Both descriptors refer to the same open file description.\n");

	printf("closing original fd=%d\n", fd);
	close(fd);

	printf("waiting before closing duplicate...\n");
	sleep(1);

	printf("closing final duplicate fd=%d\n", duplicate);
	close(duplicate);

	printf("Watch dmesg: there should be one OPEN and one final RELEASE.\n");

	return 0;
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s /dev/open_releaseX\n", argv[0]);
		return EXIT_FAILURE;
	}

	if (run_open_test(argv[1], O_RDONLY, "O_RDONLY") < 0)
		return EXIT_FAILURE;

	if (run_open_test(argv[1], O_RDWR | O_SYNC,
			  "O_RDWR | O_SYNC") < 0)
		return EXIT_FAILURE;

	if (run_open_test(argv[1], O_WRONLY | O_NONBLOCK,
			  "O_WRONLY | O_NONBLOCK") < 0)
		return EXIT_FAILURE;

	if (run_open_test(argv[1], O_WRONLY | O_APPEND,
			  "O_WRONLY | O_APPEND") < 0)
		return EXIT_FAILURE;

	if (run_dup_test(argv[1]) < 0)
		return EXIT_FAILURE;

	return EXIT_SUCCESS;
}
