CC = gcc
CFLAGS = -Wall -g 
LDFLAGS = -lpthread -levent

OBJS = libev.o csapp.o

all: libev

libev: $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

csapp.o: csapp.c
	$(CC) $(CFLAGS) -c csapp.c

libev.o: libev.c
	$(CC) $(CFLAGS) -c libev.c

clean:
	rm -f *~ *.o libev core proxy.log

