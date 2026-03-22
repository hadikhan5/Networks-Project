CC=gcc
CFLAGS=-Wall -Wextra -pthread

all: tracker peer

tracker: tracker.c common.c common.h
	$(CC) $(CFLAGS) tracker.c common.c -o tracker

peer: peer.c common.c common.h
	$(CC) $(CFLAGS) peer.c common.c -o peer

clean:
	rm -f tracker peer
