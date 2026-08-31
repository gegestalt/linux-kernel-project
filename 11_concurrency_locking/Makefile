obj-m += concurrency_locking.o

KDIR ?= /lib/modules/$(shell uname -r)/build
KERNEL_SRC ?= $(abspath ../../linux_mainline)
CHECKPATCH := $(KERNEL_SRC)/scripts/checkpatch.pl

CC ?= gcc

.PHONY: all module clean check checkpatch userspace-check

all: module stress_test

module:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

stress_test: stress_test.c
	$(CC) -Wall -Wextra -Wpedantic -O2 -pthread $< -o $@

userspace-check:
	$(CC) -Wall -Wextra -Wpedantic -Werror -pthread -fsyntax-only stress_test.c

check:
	@test -f "$(CHECKPATCH)" || \
		(echo "error: checkpatch.pl not found at $(CHECKPATCH)" && exit 1)
	perl "$(CHECKPATCH)" \
		--root="$(KERNEL_SRC)" \
		--file \
		--strict \
		--show-types \
		--color=never \
		concurrency_locking.c
	$(MAKE) userspace-check

checkpatch: check

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f stress_test
