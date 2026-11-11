CC=gcc
CFLAGS=-O2 -Wall $(shell pkg-config --cflags gtk+-3.0 alsa)
LIBS=$(shell pkg-config --libs gtk+-3.0 alsa) -lfftw3f -lcodec2 -ldl -lpthread -lm

sbitx_freedv: fdvapp.c
	$(CC) $(CFLAGS) -o sbitx_freedv fdvapp.c $(LIBS)

clean:
	rm -f sbitx_freedv
