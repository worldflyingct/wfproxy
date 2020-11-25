CC=gcc
CFLAGS=-std=gnu99 -O3

wfproxy: wfproxy.o wfhttpproxy.o
	$(CC) $(CFLAGS) -o $@ $^

wfproxy.o: wfproxy.c wfhttpproxy.h
	$(CC) $(CFLAGS) -c -o $@ wfproxy.c

wfhttpproxy.o: wfhttpproxy.c
	$(CC) $(CFLAGS) -c -o $@ wfhttpproxy.c

clean:
	rm *.o
