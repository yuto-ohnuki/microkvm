microkvm: microkvm.c
	gcc -O2 -Wall -o microkvm microkvm.c

clean:
	rm -f microkvm