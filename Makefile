all:
	gcc -Wno-incompatible-pointer-types -O3 -static-libgcc -o me main.c
	strip -s me.exe
