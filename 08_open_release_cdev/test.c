#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int fd;

	if (argc != 2) {
		fprintf(stderr, "usage: %s /dev/device\n", argv[0]);
		return 1;
	}

	printf("1. O_RDONLY\n");
	fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		perror("open O_RDONLY");
		return 1;
	}
	close(fd);

	printf("2. O_RDWR | O_SYNC\n");
	fd = open(argv[1], O_RDWR | O_SYNC);
	if (fd < 0) {
		perror("open O_RDWR | O_SYNC");
		return 1;
	}
	close(fd);

	printf("3. O_WRONLY | O_NONBLOCK\n");
	fd = open(argv[1], O_WRONLY | O_NONBLOCK);
	if (fd < 0) {
		perror("open O_WRONLY | O_NONBLOCK");
		return 1;
	}
	close(fd);

	return 0;
}
