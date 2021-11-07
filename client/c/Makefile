CC=gcc
CFLAGS=-std=gnu99 -O3
LIBS=-lm -lcrypto -lssl

wfproxy-client: wfproxy-client.o cJSON.o exception.o
	$(CC) $(CFLAGS) $(LIBS) -o $@ $^

wfproxy-client.o: wfproxy-client.c cJSON.h exception.h
	$(CC) $(CFLAGS) $(LIBS) -c -o $@ wfproxy-client.c

exception.o: exception.c exception.h
	$(CC) $(CFLAGS) $(LIBS) -c -o $@ exception.c

cJSON.o: cJSON.c cJSON.h
	$(CC) $(CFLAGS) $(LIBS) -c -o $@ cJSON.c

clean:
	rm *.o
