obj-m += open_release_cdev.o

KDIR ?= /lib/modules/$(shell uname -r)/build
KERNEL_SRC ?= $(abspath ../../linux_mainline)
CHECKPATCH := $(KERNEL_SRC)/scripts/checkpatch.pl

.PHONY: all clean check checkpatch

all: cdev_test
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

cdev_test: test.c
	$(CC) -Wall -Wextra $< -o $@

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f cdev_test

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

checkpatch: check
