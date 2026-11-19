obj-m := camv4l2.o
KDIR ?= /home/qiji_/kernel_work/kb_out_cfi
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=arm64 LLVM=1 modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=arm64 LLVM=1 clean
