all: chatlite

chatlite: chatlite.c
	$(CC) chatlite.c -o chatlite -O2 -Wall -W

clean:
	rm -f chatlite
