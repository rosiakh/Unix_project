all: server
server: server.c	
	gcc -Wall -o server server.c -lpthread
.PHONY: clean
clean:
	-rm server
