#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEVICE_PATH "/dev/race_demo"
#define MODE_PATH "/sys/class/misc/race_demo/mode"
#define COUNTER_PATH "/sys/class/misc/race_demo/counter"
#define RESET_PATH "/sys/class/misc/race_demo/reset"

#define DEFAULT_THREADS 8
#define DEFAULT_INCREMENTS 20000
#define MAX_CHUNK 65536

struct worker_arg {
	int fd;
	long increments;
};

static void *worker(void *arg_ptr)
{
	struct worker_arg *arg = arg_ptr;
	static const char scratch[MAX_CHUNK]; /* content is never read kernel-side */
	long remaining = arg->increments;

	while (remaining > 0) {
		size_t chunk = remaining > MAX_CHUNK ? MAX_CHUNK : (size_t)remaining;
		ssize_t written = write(arg->fd, scratch, chunk);

		if (written < 0) {
			perror("write");
			return NULL;
		}

		remaining -= written;
	}

	return NULL;
}

static long read_long_file(const char *path)
{
	char buf[64];
	FILE *f;
	long value;

	f = fopen(path, "r");
	if (!f) {
		perror(path);
		exit(EXIT_FAILURE);
	}

	if (!fgets(buf, sizeof(buf), f)) {
		fprintf(stderr, "failed to read %s\n", path);
		exit(EXIT_FAILURE);
	}

	fclose(f);
	value = strtol(buf, NULL, 10);

	return value;
}

static void write_string_file(const char *path, const char *value)
{
	FILE *f;

	f = fopen(path, "w");
	if (!f) {
		perror(path);
		exit(EXIT_FAILURE);
	}

	fputs(value, f);
	fclose(f);
}

int main(int argc, char **argv)
{
	int num_threads = DEFAULT_THREADS;
	long increments_per_thread = DEFAULT_INCREMENTS;
	pthread_t *threads;
	struct worker_arg arg;
	long expected;
	long actual;
	int fd;
	int i;

	if (argc >= 2)
		num_threads = atoi(argv[1]);
	if (argc >= 3)
		increments_per_thread = atol(argv[2]);

	if (num_threads <= 0 || increments_per_thread <= 0) {
		fprintf(stderr, "usage: %s [num_threads] [increments_per_thread]\n",
			argv[0]);
		return EXIT_FAILURE;
	}

	printf("mode (from sysfs) = %ld  (0=none 1=spinlock 2=mutex 3=atomic)\n",
	       read_long_file(MODE_PATH));

	write_string_file(RESET_PATH, "1");

	fd = open(DEVICE_PATH, O_WRONLY);
	if (fd < 0) {
		perror(DEVICE_PATH);
		return EXIT_FAILURE;
	}

	threads = malloc(sizeof(*threads) * num_threads);
	if (!threads) {
		perror("malloc");
		return EXIT_FAILURE;
	}

	arg.fd = fd;
	arg.increments = increments_per_thread;

	for (i = 0; i < num_threads; i++) {
		if (pthread_create(&threads[i], NULL, worker, &arg) != 0) {
			perror("pthread_create");
			return EXIT_FAILURE;
		}
	}

	for (i = 0; i < num_threads; i++)
		pthread_join(threads[i], NULL);

	close(fd);
	free(threads);

	expected = (long)num_threads * increments_per_thread;
	actual = read_long_file(COUNTER_PATH);

	printf("threads=%d increments_per_thread=%ld\n", num_threads,
	       increments_per_thread);
	printf("expected=%ld actual=%ld lost=%ld\n", expected, actual,
	       expected - actual);

	if (actual != expected) {
		printf("RACE DETECTED: %ld update(s) were lost\n",
		       expected - actual);
		return EXIT_FAILURE;
	}

	printf("no lost updates\n");

	return EXIT_SUCCESS;
}
