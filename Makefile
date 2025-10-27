CC = gcc
CFLAGS = -Wall -Wextra -std=gnu99 -O2 -D_DEFAULT_SOURCE \
	$(shell pkg-config --cflags libxml-2.0 libcurl hiredis)
LDFLAGS = $(shell pkg-config --libs libxml-2.0 libcurl hiredis)

OBJS = build/main.o \
       build/daemon.o \
       build/parser.o \
       build/sites.o \
       build/redis.o

TARGET = vpn_parser

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

build/%.o: %.c
	mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

build/parser.o: source/daemon/parser/parser.c
	mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

build/redis.o: source/daemon/storage/redis.c
	mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf build $(TARGET)

.PHONY: clean