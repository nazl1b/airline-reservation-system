CC=gcc
CFLAGS=-std=c11 -Wall -Wextra -Wpedantic -O2 -pthread

all: server client

server: server.c common.h
	$(CC) $(CFLAGS) server.c -o server

client: client.c common.h
	$(CC) $(CFLAGS) client.c -o client

clean:
	rm -f server client *.o

reset-reservations:
	: > reservations.csv