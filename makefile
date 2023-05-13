CC = gcc
CFLAGS = -std=c11 -fPIC -D_GNU_SOURCE

.PHONY: all clean default

default: all	# just because the task ask for

all: stnc.c
	$(CC) $(CFLAGS) -o stnc stnc.c

.PHONY: all clean

clean:
	rm -f *.txt stnc
