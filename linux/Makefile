CPPFLAGS = -DDEBUG -DLOG_LEVEL=LOG_DEBUG -I. -I.. -I../..
CFLAGS = -Wall -g

.PHONY: all clean

build: all

all: aws

aws: aws.o includes/sock_util.o includes/http_parser.o

aws.o: aws.c includes/sock_util.h includes/debug.h includes/util.h includes/http_parser.h

includes/sock_util.o: includes/sock_util.c includes/sock_util.h includes/debug.h includes/util.h

includes/http_parser.o: includes/http_parser.c  includes/http_parser.h
	make -C includes http_parser.o

clean:
	-rm -f *~
	-rm -f includes/*.o
	-rm -f aws

