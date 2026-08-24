obj-m += register_cdev.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M="$(PWD)" modules

mclean:
	$(MAKE) -C $(KDIR) M="$(PWD)" clean