#include "portal.h"
#include "library.h"
#include "settings.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"

static const char *TAG = "portal";

#define BIT_GOT_IP BIT0
#define BIT_FAIL BIT1
#define BIT_DNS_DONE BIT2
#define UPLOAD_TMP BOOKS_DIR "/.upload.tmp"

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static portal_status_t s_st;
static bool s_inited;
static EventGroupHandle_t s_ev;
static httpd_handle_t s_http;
static volatile bool s_dns_run;
static volatile bool s_sta_active;
static int s_retries;

portal_status_t *portal_status(void) { return &s_st; }

/* ---------------- WiFi ---------------- */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START && s_sta_active) {
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED && s_sta_active) {
            if (s_retries++ < 4) esp_wifi_connect();
            else xEventGroupSetBits(s_ev, BIT_FAIL);
        } else if (id == WIFI_EVENT_AP_STACONNECTED) {
            s_st.clients++;
        } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
            if (s_st.clients > 0) s_st.clients--;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_st.ip, sizeof(s_st.ip), IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_ev, BIT_GOT_IP);
    }
}

static void wifi_init_once(void)
{
    if (s_inited) return;
    s_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    s_inited = true;
}

/* ---------------- captive DNS: every name resolves to us ---------------- */

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGW(TAG, "dns bind failed");
        if (sock >= 0) close(sock);
        xEventGroupSetBits(s_ev, BIT_DNS_DONE);
        vTaskDelete(NULL);
        return;
    }
    struct timeval tv = {.tv_sec = 0, .tv_usec = 300000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t buf[512];
    while (s_dns_run) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12) continue;
        if ((buf[2] & 0x80) || buf[4] != 0 || buf[5] != 1) continue; /* only single queries */
        int q = 12;
        while (q < n && buf[q]) q += buf[q] + 1;
        if (q + 5 > n) continue;
        uint16_t qtype = (uint16_t)(buf[q + 1] << 8 | buf[q + 2]);
        int qend = q + 5;
        buf[2] = 0x81; /* response, recursion desired */
        buf[3] = 0x80; /* recursion available, no error */
        buf[6] = 0; buf[7] = qtype == 1 ? 1 : 0; /* answers */
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        int len = qend;
        if (qtype == 1) {
            uint8_t ans[16] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1};
            memcpy(buf + len, ans, sizeof(ans));
            len += sizeof(ans);
        }
        sendto(sock, buf, len, 0, (struct sockaddr *)&from, fl);
    }
    close(sock);
    xEventGroupSetBits(s_ev, BIT_DNS_DONE);
    vTaskDelete(NULL);
}

/* ---------------- HTTP helpers ---------------- */

static void url_decode(char *s)
{
    char *w = s;
    for (; *s; s++) {
        if (*s == '+') *w++ = ' ';
        else if (*s == '%' && s[1] && s[2]) {
            char h[3] = {s[1], s[2], 0};
            *w++ = (char)strtol(h, NULL, 16);
            s += 2;
        } else *w++ = *s;
    }
    *w = 0;
}

static bool query_name(httpd_req_t *req, char *name, size_t sz)
{
    char q[400];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
    if (httpd_query_key_value(q, "name", name, sz) != ESP_OK) return false;
    url_decode(name);
    return name[0] != 0;
}

static void json_str(char *out, size_t sz, const char *s)
{
    size_t j = 0;
    if (sz < 3) return;
    out[j++] = '"';
    for (; *s && j + 7 < sz; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[j++] = '\\'; out[j++] = (char)c; }
        else if (c < 0x20) j += snprintf(out + j, sz - j, "\\u%04x", c);
        else out[j++] = (char)c;
    }
    out[j++] = '"';
    out[j] = 0;
}

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    char esc[160], body[200];
    json_str(esc, sizeof(esc), msg);
    snprintf(body, sizeof(body), "{\"error\":%s}", esc);
    return send_json(req, status, body);
}

/* the upload page accepts EPUB, plain text and Markdown only */
static bool upload_type_ok(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return !strcasecmp(dot, ".epub") || !strcasecmp(dot, ".txt") || !strcasecmp(dot, ".md") ||
           !strcasecmp(dot, ".markdown");
}

/* ---------------- handlers ---------------- */

static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t h_books(httpd_req_t *req)
{
    lib_entry_t *list;
    int n = library_scan(&list);
    uint64_t total = 0, fr = 0;
    storage_info(&total, &fr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char item[400], esc[220];
    httpd_resp_sendstr_chunk(req, "{\"books\":[");
    for (int i = 0; i < n; i++) {
        json_str(esc, sizeof(esc), list[i].name);
        snprintf(item, sizeof(item), "%s{\"name\":%s,\"size\":%lu,\"pct\":%d}", i ? "," : "", esc,
                 (unsigned long)list[i].size, list[i].pct);
        httpd_resp_sendstr_chunk(req, item);
    }
    library_free(list);
    char ssid[80], home[80];
    json_str(ssid, sizeof(ssid), s_st.ssid);
    json_str(home, sizeof(home), g_set.sta_ssid);
    snprintf(item, sizeof(item), "],\"total\":%llu,\"free\":%llu,\"mode\":\"%s\",\"ssid\":%s,\"home\":%s}",
             total, fr, s_st.mode == PORTAL_STA ? "sta" : "ap", ssid, home);
    httpd_resp_sendstr_chunk(req, item);
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t h_upload(httpd_req_t *req)
{
    char name[MAX_NAME + 1];
    if (!query_name(req, name, sizeof(name))) return send_error(req, "400 Bad Request", "Missing file name");
    if (!upload_type_ok(name) || !library_name_ok(name))
        return send_error(req, "400 Bad Request", "Only EPUB, TXT and Markdown files");
    size_t total = req->content_len;
    if (total == 0) return send_error(req, "400 Bad Request", "Empty file");
    uint64_t cap = 0, fr = 0;
    storage_info(&cap, &fr);
    if ((uint64_t)total + 32768 > fr) return send_error(req, "507 Insufficient Storage", "Not enough free space on the device");

    FILE *f = fopen(UPLOAD_TMP, "wb");
    if (!f) return send_error(req, "500 Internal Server Error", "Cannot create file");
    char *buf = malloc(8192);
    if (!buf) {
        fclose(f);
        return send_error(req, "500 Internal Server Error", "Out of memory");
    }
    snprintf(s_st.up_name, sizeof(s_st.up_name), "%s", name);
    s_st.up_total = total;
    s_st.up_recv = 0;
    s_st.uploading = true;
    size_t remaining = total;
    int timeouts = 0;
    const char *fail = NULL;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining < 8192 ? remaining : 8192);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts < 5) continue;
            fail = "Upload timed out";
            break;
        }
        if (r <= 0) { fail = "Connection lost"; break; }
        timeouts = 0;
        if (fwrite(buf, 1, r, f) != (size_t)r) { fail = "Write failed (storage full?)"; break; }
        remaining -= r;
        s_st.up_recv += r;
    }
    free(buf);
    bool closed_ok = fclose(f) == 0;
    s_st.uploading = false;
    if (!fail && !closed_ok) fail = "Write failed (storage full?)";
    if (fail) {
        remove(UPLOAD_TMP);
        snprintf(s_st.message, sizeof(s_st.message), "Failed: %s", fail);
        return send_error(req, "500 Internal Server Error", fail);
    }
    char path[160];
    snprintf(path, sizeof(path), BOOKS_DIR "/%s", name);
    remove(path);
    library_forget(name);
    if (rename(UPLOAD_TMP, path) != 0) {
        remove(UPLOAD_TMP);
        return send_error(req, "500 Internal Server Error", "Cannot save file");
    }
    s_st.received++;
    s_st.changes++;
    snprintf(s_st.message, sizeof(s_st.message), "Received %s", name);
    ESP_LOGI(TAG, "uploaded %s (%u bytes)", name, (unsigned)total);
    return send_json(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t h_delete(httpd_req_t *req)
{
    char name[MAX_NAME + 1];
    if (!query_name(req, name, sizeof(name)) || !library_name_ok(name))
        return send_error(req, "400 Bad Request", "Bad file name");
    char path[160];
    snprintf(path, sizeof(path), BOOKS_DIR "/%s", name);
    if (remove(path) != 0) return send_error(req, "404 Not Found", "No such book");
    library_forget(name);
    if (!strcmp(g_set.last_book, name)) {
        g_set.last_book[0] = 0;
        settings_save();
    }
    s_st.changes++;
    snprintf(s_st.message, sizeof(s_st.message), "Deleted %s", name);
    return send_json(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t h_download(httpd_req_t *req)
{
    char name[MAX_NAME + 1];
    if (!query_name(req, name, sizeof(name)) || !library_name_ok(name))
        return send_error(req, "400 Bad Request", "Bad file name");
    char path[160];
    snprintf(path, sizeof(path), BOOKS_DIR "/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) return send_error(req, "404 Not Found", "No such book");
    httpd_resp_set_type(req, "application/octet-stream");
    char *buf = malloc(4096);
    if (!buf) {
        fclose(f);
        return send_error(req, "500 Internal Server Error", "Out of memory");
    }
    size_t r;
    esp_err_t err = ESP_OK;
    while ((r = fread(buf, 1, 4096, f)) > 0) {
        err = httpd_resp_send_chunk(req, buf, r);
        if (err != ESP_OK) break;
    }
    free(buf);
    fclose(f);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

static esp_err_t h_wifi(httpd_req_t *req)
{
    char body[256] = "";
    int len = req->content_len < sizeof(body) - 1 ? req->content_len : sizeof(body) - 1;
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) break;
        got += r;
    }
    body[got] = 0;
    char ssid[100] = "", pass[140] = "";
    httpd_query_key_value(body, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(body, "pass", pass, sizeof(pass));
    url_decode(ssid);
    url_decode(pass);
    if (strlen(ssid) > 32 || strlen(pass) > 64) return send_error(req, "400 Bad Request", "Name or password too long");
    snprintf(g_set.sta_ssid, sizeof(g_set.sta_ssid), "%s", ssid);
    snprintf(g_set.sta_pass, sizeof(g_set.sta_pass), "%s", pass);
    settings_save();
    return send_json(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t h_not_found(httpd_req_t *req, httpd_err_code_t code)
{
    /* captive portal: send every unknown URL to the upload page */
    char loc[40];
    snprintf(loc, sizeof(loc), "http://%s/", s_st.ip[0] ? s_st.ip : "192.168.4.1");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", loc);
    return httpd_resp_send(req, NULL, 0);
}

static void http_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 10;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 6;
    cfg.recv_wait_timeout = 15;
    cfg.send_wait_timeout = 15;
    if (httpd_start(&s_http, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "http server failed");
        s_http = NULL;
        return;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_index},
        {.uri = "/api/books", .method = HTTP_GET, .handler = h_books},
        {.uri = "/api/upload", .method = HTTP_POST, .handler = h_upload},
        {.uri = "/api/delete", .method = HTTP_POST, .handler = h_delete},
        {.uri = "/api/download", .method = HTTP_GET, .handler = h_download},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = h_wifi},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(s_http, &uris[i]);
    httpd_register_err_handler(s_http, HTTPD_404_NOT_FOUND, h_not_found);
}

/* ---------------- start / stop ---------------- */

static bool try_station(void)
{
    wifi_config_t sc = {0};
    snprintf((char *)sc.sta.ssid, sizeof(sc.sta.ssid), "%s", g_set.sta_ssid);
    snprintf((char *)sc.sta.password, sizeof(sc.sta.password), "%s", g_set.sta_pass);
    sc.sta.threshold.authmode = g_set.sta_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    xEventGroupClearBits(s_ev, BIT_GOT_IP | BIT_FAIL);
    s_retries = 0;
    s_sta_active = true;
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &sc);
    esp_wifi_start();
    EventBits_t bits = xEventGroupWaitBits(s_ev, BIT_GOT_IP | BIT_FAIL, pdTRUE, pdFALSE, pdMS_TO_TICKS(15000));
    if (bits & BIT_GOT_IP) return true;
    s_sta_active = false;
    esp_wifi_disconnect();
    esp_wifi_stop();
    return false;
}

void portal_start(bool try_home)
{
    wifi_init_once();
    s_st.mode = PORTAL_CONNECTING;
    s_st.received = 0;
    s_st.message[0] = 0;
    s_st.ip[0] = 0;
    s_st.clients = 0;
    if (try_home && g_set.sta_ssid[0]) {
        snprintf(s_st.ssid, sizeof(s_st.ssid), "%s", g_set.sta_ssid);
        if (try_station()) {
            s_st.mode = PORTAL_STA;
            s_st.pass[0] = 0;
            http_start();
            ESP_LOGI(TAG, "joined %s, open http://%s", s_st.ssid, s_st.ip);
            return;
        }
        snprintf(s_st.message, sizeof(s_st.message), "Could not join %s", g_set.sta_ssid);
    }
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_st.ssid, sizeof(s_st.ssid), "%s-%02X%02X", CONFIG_EREADER_AP_SSID_PREFIX, mac[4], mac[5]);
    snprintf(s_st.pass, sizeof(s_st.pass), "%s", CONFIG_EREADER_AP_PASSWORD);
    wifi_config_t ac = {0};
    memcpy(ac.ap.ssid, s_st.ssid, strlen(s_st.ssid));
    ac.ap.ssid_len = strlen(s_st.ssid);
    snprintf((char *)ac.ap.password, sizeof(ac.ap.password), "%s", s_st.pass);
    ac.ap.channel = CONFIG_EREADER_AP_CHANNEL;
    ac.ap.max_connection = 4;
    ac.ap.authmode = strlen(s_st.pass) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    if (ac.ap.authmode == WIFI_AUTH_OPEN) s_st.pass[0] = 0;
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ac);
    esp_wifi_start();
    snprintf(s_st.ip, sizeof(s_st.ip), "192.168.4.1");
    s_dns_run = true;
    xEventGroupClearBits(s_ev, BIT_DNS_DONE);
    xTaskCreate(dns_task, "dns", 3072, NULL, 4, NULL);
    http_start();
    s_st.mode = PORTAL_AP;
    ESP_LOGI(TAG, "hotspot %s ready", s_st.ssid);
}

void portal_stop(void)
{
    if (s_st.mode == PORTAL_OFF) return;
    if (s_http) {
        httpd_stop(s_http);
        s_http = NULL;
    }
    if (s_dns_run) {
        s_dns_run = false;
        xEventGroupWaitBits(s_ev, BIT_DNS_DONE, pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
    }
    s_sta_active = false;
    esp_wifi_disconnect();
    esp_wifi_stop();
    remove(UPLOAD_TMP);
    s_st.mode = PORTAL_OFF;
    s_st.uploading = false;
}
