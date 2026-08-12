CC      = gcc
CFLAGS  = -O2 -Wall -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-implicit-function-declaration -Wno-implicit-int

.PHONY: all clean

all: code

code: main.c buddy.c buddy.h utils.h
	$(CC) $(CFLAGS) -o code main.c buddy.c

clean:
	rm -f code test
