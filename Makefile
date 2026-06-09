microkvm: microkvm.c uart.c virtio_mmio.c uart.h virtio_mmio.h
	gcc -O2 -Wall -o microkvm microkvm.c uart.c virtio_mmio.c -lpthread

clean:
	rm -f microkvm