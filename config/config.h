// config.h
#ifndef CONFIG_H
#define CONFIG_H

// Интервал между сканированиями (секунды)
#define SCAN_INTERVAL 3600

// Таймаут HTTP-запроса (секунды)
#define HTTP_TIMEOUT 30

// Макс. серверов от одного сайта (защита от флуда)
#define MAX_SERVERS_PER_SITE 50

// TTL записей в Redis (секунды)
#define REDIS_TTL 86400  // 24 часа

// Путь к директории ресурсов (если понадобится)
#define RESOURCE_DIR "source/daemon/resource"

#endif