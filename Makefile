obj-m += edu_drv.o
KDIR ?= /lib/modules/$(shell uname -r)/build

all: test
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

test: test.c edu_uapi.h
	$(CC) -static -O2 -Wall -o $@ test.c

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -rf test rootfs initramfs.gz
ccflags-y += -g
