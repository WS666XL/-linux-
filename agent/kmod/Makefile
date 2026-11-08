# 内核模块外部构建：make -C <内核树> M=$(PWD) modules
# 内核树默认指向 WSL2 自建内核（含 Module.symvers）
KDIR ?= $(HOME)/wsl-kernel

obj-m += cpu_stat_monitor.o softirq_monitor.o

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all clean
