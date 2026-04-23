CC=gcc
CFLAGS=-Wall -Wextra -pthread

all: tracker peer peer_dirs

tracker: tracker.c common.c common.h
	$(CC) $(CFLAGS) tracker.c common.c -o tracker

peer: peer.c common.c common.h
	$(CC) $(CFLAGS) peer.c common.c -o peer

# Build peer executable into peer1/, peer2/, peer3/ folders (per project spec)
peer_dirs: peer
	mkdir -p peer1 peer2 peer3
	cp peer peer1/peer
	cp peer peer2/peer
	cp peer peer3/peer

clean:
	rm -f tracker peer
	rm -f peer1/peer peer2/peer peer3/peer
