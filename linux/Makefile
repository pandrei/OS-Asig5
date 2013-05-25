CPPFLAGS = -DDEBUG -DLOG_LEVEL=LOG_DEBUG -I. -I.. -I../..
CFLAGS = -Wall -g

.PHONY: all clean

build: all

all: main_http_server

main_http_server: main_http_server.o sock_util.o

main_http_server.o: main_http_server.c sock_util.h debug.h util.h

sock_util.o: sock_util.c sock_util.h debug.h util.h

http-parser/http_parser.o: http-parser/http_parser.c  http-parser/http_parser.h
	make -C http_parser/http_parser.o

clean:
	-rm -f *~
	-rm -f *.o
	-rm -f main_http_server
