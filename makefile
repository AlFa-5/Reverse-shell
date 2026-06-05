
CC      = musl-gcc
CFLAGS  = -Wall -Wextra -O2 -static -s -fPIE -fno-pie
LDFLAGS =

all: serveur client

# Compilation du serveur
serveur: serveur.c protocol.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o serveur serveur.c

# Compilation du client
client: client.c protocol.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o client client.c

# Nettoyage
clean:
	rm -f serveur client

.PHONY: all clean check
