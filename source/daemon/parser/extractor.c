// source/daemon/parser/extractor.c
#include <libxml/HTMLparser.h>
#include <libxml/xpath.h>
#include <string.h>
#include <stdio.h>
#include "../../../database/redis/utils/redis_store.h"

/**
 * @brief Извлекает данные о серверах с vpngate.net и сохраняет в Redis.
 * @param html — тело HTML-страницы
 * @param redis_ctx — подключение к Redis
 */
void extract_vpngate_servers(const char *html, redisContext *redis_ctx) {
    htmlDocPtr doc = htmlReadDoc((xmlChar*)html, NULL, NULL,
                                 HTML_PARSE_RECOVER | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    if (!doc) return;

    // XPath к строкам таблицы (пропускаем заголовок)
    xmlXPathContextPtr xpathCtx = xmlXPathNewContext(doc);
    xmlXPathObjectPtr xpathObj = xmlXPathEvalExpression(
        (xmlChar*)"//table[@id='vg_hosts_table_id']//tr[position()>1]", xpathCtx);

    if (xpathObj && xpathObj->nodesetval) {
        for (int i = 0; i < xpathObj->nodesetval->nodeNr; i++) {
            xmlNodePtr row = xpathObj->nodesetval->nodeTab[i];
            xmlNodePtr cell = row->children;

            // Пропускаем пустые ячейки
            while (cell && cell->type != XML_ELEMENT_NODE) cell = cell->next;
            if (!cell) continue;

            // IP находится в первой ячейке (внутри <span>)
            xmlChar *ip = xmlNodeGetContent(cell->children);
            if (!ip || strlen((char*)ip) < 7) {
                xmlFree(ip);
                continue;
            }

            // Переходим к порту (3-я ячейка)
            int col = 0;
            xmlNodePtr cur = row->children;
            while (cur && col < 3) {
                if (cur->type == XML_ELEMENT_NODE) col++;
                if (col < 3) cur = cur->next;
            }

            int port = 0;
            if (cur && cur->children) {
                xmlChar *port_str = xmlNodeGetContent(cur->children);
                port = atoi((char*)port_str);
                xmlFree(port_str);
            }

            // Страна — 2-я ячейка
            col = 0;
            cur = row->children;
            while (cur && col < 2) {
                if (cur->type == XML_ELEMENT_NODE) col++;
                if (col < 2) cur = cur->next;
            }
            xmlChar *country = NULL;
            if (cur && cur->children && cur->children->next) {
                country = xmlGetProp(cur->children->next, (xmlChar*)"alt");
            }

            // Скорость — 4-я ячейка
            col = 0;
            cur = row->children;
            while (cur && col < 4) {
                if (cur->type == XML_ELEMENT_NODE) col++;
                if (col < 4) cur = cur->next;
            }
            double score = 0.0;
            if (cur && cur->children) {
                xmlChar *speed_str = xmlNodeGetContent(cur->children);
                // Убираем " Mbps"
                char *end = strstr((char*)speed_str, " ");
                if (end) *end = '\0';
                score = atof((char*)speed_str);
                xmlFree(speed_str);
            }

            // Ссылка на .ovpn — ищем в 7-й ячейке
            col = 0;
            cur = row->children;
            while (cur && col < 7) {
                if (cur->type == XML_ELEMENT_NODE) col++;
                if (col < 7) cur = cur->next;
            }
            xmlChar *ovpn_url = NULL;
            if (cur) {
                xmlXPathContextPtr cellCtx = xmlXPathNewContext(doc);
                cellCtx->node = cur;
                xmlXPathObjectPtr linkObj = xmlXPathEvalExpression(
                    (xmlChar*)".//a[contains(@href, '.ovpn')]/@href", cellCtx);
                if (linkObj && linkObj->nodesetval && linkObj->nodesetval->nodeTab[0]) {
                    ovpn_url = xmlNodeGetContent(linkObj->nodesetval->nodeTab[0]);
                }
                xmlXPathFreeObject(linkObj);
                xmlXPathFreeContext(cellCtx);
            }

            if (ovpn_url && port > 0) {
                char full_url[1024];
                snprintf(full_url, sizeof(full_url), "https://www.vpngate.net%s", ovpn_url);

                // Определяем протокол по порту (грубая эвристика)
                const char *proto = (port == 443 || port == 53) ? "tcp" : "udp";

                redis_save_vpn_server(
                    redis_ctx, "vpngate",
                    (char*)ip, port, proto,
                    country ? (char*)country : "??",
                    score, full_url
                );
            }

            xmlFree(ip);
            if (country) xmlFree(country);
            if (ovpn_url) xmlFree(ovpn_url);
        }
    }

    xmlXPathFreeObject(xpathObj);
    xmlXPathFreeContext(xpathCtx);
    xmlFreeDoc(doc);
}