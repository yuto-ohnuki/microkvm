obj-m := microkvm_pci.o
KDIR := $(HOME)/linux-src_v2

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean