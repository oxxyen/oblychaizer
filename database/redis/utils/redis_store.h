// source/daemon/storage/redis_store.h
#ifndef REDIS_STORE_H
#define REDIS_STORE_H

#include <hiredis/hiredis.h>

/**
 * @brief Инициализирует подключение к локальному Redis.
 * @return redisContext* или NULL при ошибке.
 */
redisContext* redis_connect(void);

/**
 * @brief Сохраняет информацию о VPN-сервере в Redis.
 *
 * Ключ: vpn:servers:<site>:<hash>
 * Поля: ip, port, protocol, country, score, last_seen, config_url
 *
 * @param c — подключение к Redis
 * @param site — имя сайта (например, "vpngate")
 * @param ip — IP-адрес сервера
 * @param port — порт (число)
 * @param proto — "tcp" или "udp"
 * @param country — код страны (например, "US")
 * @param score — числовой рейтинг (например, скорость в Мбит/с)
 * @param config_url — полный URL к .ovpn файлу
 * @return 0 при успехе, -1 при ошибке
 */
int redis_save_vpn_server(
    redisContext *c,
    const char *site,
    const char *ip,
    int port,
    const char *proto,
    const char *country,
    double score,
    const char *config_url
);

#endif