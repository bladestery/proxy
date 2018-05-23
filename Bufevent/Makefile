CC = gcc
CFLAGS = -Wall -g 
LDFLAGS = -lpthread -levent

OBJS = bufev.o csapp.o

all: bufev

bufev: $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

csapp.o: csapp.c
	$(CC) $(CFLAGS) -c csapp.c

bufev.o: bufev.c
	$(CC) $(CFLAGS) -c bufev.c

clean:
	rm -f *~ *.o bufev core

