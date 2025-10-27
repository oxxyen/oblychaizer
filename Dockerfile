# Используем минимальный образ с GCC
FROM alpine:latest

# Устанавливаем необходимые пакеты
RUN apk --no-cache add \
    gcc \
    musl-dev \
    make \
    git \
    && rm -rf /var/cache/apk/*

# Копируем весь проект
COPY . /app

# Переходим в папку parser и собираем
WORKDIR /app/parser

# Компилируем (предполагается, что Makefile есть)
RUN make

# Возвращаемся в корень и копируем бинарник в bin/
WORKDIR /app
RUN mkdir -p bin && cp parser/bin/parser bin/main

# Устанавливаем рабочую директорию
WORKDIR /app

# Запускаем основной исполняемый файл
CMD ["./bin/main"]