all: chatlite chatlite-client

chatlite: chatlite.c
	$(CC) chatlite.c -o chatlite -O2 -Wall -W

chatlite-client: chatlite_client.c
	$(CC) chatlite_client.c -o chatlite-client -O2 -Wall -W

clean:
	rm -f chatlite chatlite-client
