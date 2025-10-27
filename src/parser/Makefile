CC=gcc
CFLAGS=-std=c11 -O2 -Wall -Wextra -pthread
LIBS=-lcurl -lhiredis

all: parser

parser: parser.c
	$(CC) $(CFLAGS) parser.c -o parser $(LIBS)

clean:
	rm -f parser
