microkvm: microkvm.c uart.c uart.h
	gcc -O2 -Wall -o microkvm microkvm.c uart.c -lpthread

clean:
	rm -f microkvm