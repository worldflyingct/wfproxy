CC=gcc
CFLAGS=-std=gnu99 -O3

wfproxy: wfproxy.o wfhttp.o
	$(CC) $(CFLAGS) -o $@ $^

wfproxy.o: wfproxy.c wfhttp.h
	$(CC) $(CFLAGS) -c -o $@ wfproxy.c

wfhttp.o: wfhttp.c
	$(CC) $(CFLAGS) -c -o $@ wfhttp.c

clean:
	rm *.o
