microkvm: microkvm.c guest.bin
	gcc -O2 -Wall -o microkvm microkvm.c

guest.bin: guest.S
	as --32 -o guest.o guest.S
	ld -m elf_i386 -Ttext=0x0 --oformat binary -o guest.bin guest.o

clean:
	rm -f microkvm guest.o guest.bin