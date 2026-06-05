CC = gcc
CFLAGS = -Wall -Wextra -O2 -static
LDFLAGS = -static

all: serveur client

serveur: serveur.c protocol.h
	$(CC) $(CFLAGS) -o serveur serveur.c $(LDFLAGS)

client: client.c protocol.h
	$(CC) $(CFLAGS) -o client client.c $(LDFLAGS)

clean:
	rm -f serveur client

.PHONY: all clean