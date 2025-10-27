// source/daemon/parser/sites.h
#ifndef SITES_H
#define SITES_H

/**
 * @brief Список поддерживаемых сайтов с метаданными.
 * Расширяем: просто добавьте новую запись.
 */
typedef struct {
    const char *name;      // Уникальное имя (для логов и Redis-ключей)
    const char *url;       // URL для парсинга
    const char *base_url;  // База для относительных ссылок
} vpn_site_t;

static const vpn_site_t SUPPORTED_SITES[] = {
    {"vpngate", "https://www.vpngate.net/en/", "https://www.vpngate.net"},
    {"vpnbook", "https://www.vpnbook.com/freevpn", "https://www.vpnbook.com"},

};

#define SITE_COUNT (sizeof(SUPPORTED_SITES) / sizeof(SUPPORTED_SITES[0]))

#endif