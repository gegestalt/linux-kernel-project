obj-m += open_release_cdev.o

KDIR ?= /lib/modules/$(shell uname -r)/build
KERNEL_SRC ?= $(abspath ../../linux_mainline)
CHECKPATCH := $(KERNEL_SRC)/scripts/checkpatch.pl

CC ?= gcc

.PHONY: all module clean check checkpatch userspace-check

all: module cdev_test

module:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

cdev_test: test.c
	$(CC) -Wall -Wextra -Wpedantic -O2 $< -o $@

userspace-check:
	$(CC) -Wall -Wextra -Wpedantic -Werror -fsyntax-only test.c

check:
	@test -f "$(CHECKPATCH)" || \
		(echo "error: checkpatch.pl not found at $(CHECKPATCH)" && exit 1)
	perl "$(CHECKPATCH)" \
		--root="$(KERNEL_SRC)" \
		--file \
		--strict \
		--show-types \
		--color=never \
		open_release_cdev.c
	$(MAKE) userspace-check

checkpatch: check

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f cdev_test
