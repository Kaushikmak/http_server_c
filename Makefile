CC = g++
CFLAGS = -g -Wall
LDFLAGS = -lpthread

all: proxy

# final executable
proxy: server.o proxy_parse.o logger.o
	$(CC) $(CFLAGS) -o proxy server.o proxy_parse.o logger.o $(LDFLAGS)

# object files
server.o: server.c proxy_parse.h logger.h
	$(CC) $(CFLAGS) -c server.c

proxy_parse.o: proxy_parse.c proxy_parse.h
	$(CC) $(CFLAGS) -c proxy_parse.c

logger.o: logger.c logger.h
	$(CC) $(CFLAGS) -c logger.c

clean:
	rm -f proxy *.o

tar:
	tar -cvzf ass1.tgz server.c README Makefile proxy_parse.c proxy_parse.h logger.c logger.h