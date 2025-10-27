CC = gcc
CFLAGS = -Wall -Wextra -std=gnu99 -O2 $(shell pkg-config --cflags libxml-2.0 libcurl hiredis)
LDFLAGS = $(shell pkg-config --libs libxml-2.0 libcurl hiredis)

# Все исходники
SRCS := main.c \
	source/daemon/daemon.c \
	source/daemon/storage/redis_store.c \
	source/daemon/parser/extractor.c

# Генерация объектных файлов с сохранением структуры
OBJS := $(SRCS:%.c=build/%.o)

TARGET = vpn_parser

# Правило сборки
$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

# Общее правило компиляции .c → .o с созданием директорий
build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# Очистка
clean:
	rm -rf build $(TARGET)

.PHONY: clean