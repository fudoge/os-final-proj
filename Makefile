client: client.c
	gcc -pthread -o client client.c
server: server.c
	gcc -pthread -o server server.c

clean:
	rm server client
