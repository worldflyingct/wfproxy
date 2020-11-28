CC=gcc
CFLAGS=-std=gnu99 -O3
LIBS=-lm -lcrypto -lssl

wfproxy-client: wfproxy-client.o cJSON.o
	$(CC) $(CFLAGS) $(LIBS) -o $@ $^

wfproxy-client.o: wfproxy-client.c cJSON.h
	$(CC) $(CFLAGS) $(LIBS) -c -o $@ wfproxy-client.c

cJSON.o: cJSON.c cJSON.h
	$(CC) $(CFLAGS) $(LIBS) -c -o $@ cJSON.c

clean:
	rm *.o
