all: chatlite

smallchat: chatlite.c
	$(CC) chatlite.c -o chatlite -O2 -Wall -W

clean:
	rm -f chatlite
