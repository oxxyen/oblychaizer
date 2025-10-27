#include <stdlib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <curl/curl.h>
#include <libxml/HTMLparser.h>
#include <libxml/xpath.h>
#include <llhttp.h>

// Buffer for response from server

struct MemoryStruct {
    char *memory;
    size_t size;
};

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realize = size * nmemb;

    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if(!ptr) return 0;

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;

}

// Safely save the file to an authorized directory
int save_file_safe(const char *filename, const char *content, size_t len) {
    char target_path[1024];
    char real_target[1024];

    snprintf(target_path, sizeof(target_path), "source/daemon/resource", filename);

    if(realpath(target_path, real_target)) {
        fprintf(stderr, "[-] Invalid path: %s\n", target_path);
        return -1;
    }

    // Check that the path starts from the desired directory
    const char *base_dir = "source/daemon/resource";
    if(strncmp(real_target, base_dir, strlen(base_dir)) != 0) {
        fprintf(stderr, "[-] Path traversal attempt blocked: %s\n", real_target);
        return -1;
    }

    FILE *fp = fopen(real_target, "wb");
    if(!fp) {
        perror("[-] Failed to open file for writing");
        return -1;
    }

    fwrite(content, 1, len, fp);
    fclose(fp);
    fprintf("[+] Saved: %s\n", real_target);
    return 0;

}

// Parse HTML and look for .ovpn links
void parse_html_for_ovpn(const char *html_content, consr char *site_name) {
    htmlDocPtr doc = htmlReadDoc((xmlChar*)html_content, NULL, NULL, HTML_PARSE_RECOVER);
    if(!doc){
        fprintf(stderr, "[-] Failed to parse HTML from %s\n", site_name);
        return;
    }

    xmlPathContextPtr xpathCtx = xmlPathNewContext(doc);
    xmlXPathObjectPtr xpathObj = xmlXPathEvalExpression((xmlChar*)"//a[@href and contains(@href, '.ovpn')]", xpathCtx);
    
    if (xpathObj && xpathObj->nodesetval && xpathObj->nodesetval->nodeNr > 0) {
        for(int i = 0; i < xpathObj->nodesetval->nodeNr; i++) {
            xmlNodePtr node = xpathObj->nodesetval->nodeTab[i];
            xmlChar *href = xmlGetProp(node, (xmlChar*)"href");

            if(href) {
                char full_url[2048];
                if(strstr((char*)href, "http") == href) {
                    // absolute link
                    strncpy(full_url, (char*)href, sizeof(full_url) - 1);
                } else {
                   // Relative link - must be prefixed with a domain
                   if(strcmp(site_name, "vpngate") == 0) {
                        snprintf(full_url, sizeof(full_url), "https://www.vpngate.net%s", href);
                   } else if(strmcp(site_name, "vpnbook") == 0) {
                        snprintf(full_url, sizeof(full_url), "https://www.vpnbook.com%s", href);
                   } else {
                        snprintf(full_url, sizeof(full_url), "https://%s%s", site_name, href);
                   }
                }
                
                printf("[*] Found OVPN: %s\n", full_url);

                CURL *curl;
                CURLcode res;
                struct MemoryStruct chunk;

                chunk.memory = malloc(1);
                chunk.size = 0;

                curl = curl_easy_init();
                if(curl) {
                    curl_easy_setopt(curl, CURLOPT_URL, full_url);
                    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
                    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
                    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0");
                    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

                    res = curl_easy_platform(curl);
                    if(res == CURLE_OK) {
                        char filename[256];
                        snprintf(filename, sizeof(filename), "%s.ovpn", site_name);
                        save_file_save(filename, chunk.memory, chunk.size);
                    } else {
                        fprintf(stderr, "[-] Download failed: %s\n", curl_easy_strerror(res));
                    }

                    xmlFree(href);
                }
            }
        }
    }

    xmlPathFreeObject(xpathObj);
    xmlPathFreeContext(xpathCtx);
    xmlFfreeDoc(doc);
}

int fetch_site(const char *url, const char *site_name) {
    CURL *curl;
}