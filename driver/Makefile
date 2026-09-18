obj-m := microkvm_pci.o
KDIR := $(HOME)/linux-src

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
