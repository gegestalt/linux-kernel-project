obj-m += register_cdev.o

KDIR ?= /lib/modules/$(shell uname -r)/build
KERNEL_SRC ?= $(abspath ../../linux_mainline)
CHECKPATCH := $(KERNEL_SRC)/scripts/checkpatch.pl

.PHONY: all clean check checkpatch

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

check:
	@test -f "$(CHECKPATCH)" || \
		(echo "error: checkpatch.pl not found at $(CHECKPATCH)" && exit 1)
	perl "$(CHECKPATCH)" \
		--root="$(KERNEL_SRC)" \
		--file \
		--strict \
		--show-types \
		--color=never \
		register_cdev.c

checkpatch: check
