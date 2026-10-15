// main/web_cfg.c —— 见 web_cfg.h。
#include "web_cfg.h"

#include "app_status.h"
#include "form_parse.h"
#include "log_ring.h"
#include "usb_link.h"
#include "web_page.h"
#include "wifi_sta.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "web_cfg";

#define FORM_BODY_MAX  256
#define LOG_CHUNK      1024
#define LOG_TOTAL      4096

static httpd_handle_t s_server;

// 只服务 USB 链路上的电脑:拒绝来自 Wi-Fi 侧(局域网其他设备)的访问,
// 并校验 Host 头,防 DNS rebinding。
static bool request_allowed(httpd_req_t *req)
{
    struct sockaddr_storage local;
    socklen_t len = sizeof(local);
    int fd = httpd_req_to_sockfd(req);
    if (getsockname(fd, (struct sockaddr *)&local, &len) != 0) return false;
    uint32_t local_ip;
    if (local.ss_family == AF_INET) {
        local_ip = ((const struct sockaddr_in *)&local)->sin_addr.s_addr;
    } else if (local.ss_family == AF_INET6) {
        // httpd 在启用 IPv6 时监听双栈 socket,IPv4 连接的本端地址是
        // IPv4 映射形式 ::ffff:a.b.c.d,取最后 4 字节。
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)&local;
        const uint32_t *w = in6->sin6_addr.un.u32_addr;
        if (w[0] != 0 || w[1] != 0 || w[2] != PP_HTONL(0x0000FFFFUL)) return false;
        local_ip = w[3];
    } else {
        return false;
    }
    if (local_ip != usb_link_local_ip()) return false;

    char host[24];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) return false;
    char *colon = strchr(host, ':');
    if (colon) *colon = '\0';
    return strcmp(host, USB_LINK_LOCAL_IP_STR) == 0;
}

// 修改类请求额外要求自定义头:跨站网页无法在不经 CORS 预检的情况下携带它,
// 本服务不响应预检,因此其他网站不能偷偷改 Wi-Fi 配置。
static bool mutation_allowed(httpd_req_t *req)
{
    return request_allowed(req) && httpd_req_get_hdr_value_len(req, "X-Passport") > 0;
}

static esp_err_t deny(httpd_req_t *req)
{
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "forbidden");
    return ESP_OK;
}

static void ip_to_str(uint32_t addr, char *out, size_t size)
{
    if (addr == 0) {
        out[0] = '\0';
        return;
    }
    struct in_addr a = { .s_addr = addr };
    inet_ntoa_r(a, out, size);
}

static esp_err_t page_get(httpd_req_t *req)
{
    if (!request_allowed(req)) return deny(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, WEB_PAGE_HTML, sizeof(WEB_PAGE_HTML) - 1);
}

static esp_err_t status_get(httpd_req_t *req)
{
    if (!request_allowed(req)) return deny(req);
    app_status_t st;
    app_status_get(&st);
    uint32_t rx, tx, drops;
    usb_link_counters(&rx, &tx, &drops);

    char ssid[33 * 6 + 1], try_ssid[33 * 6 + 1];
    char sta_ip[16], host_ip[16], dns[16];
    json_escape(st.ssid, ssid, sizeof(ssid));
    json_escape(st.try_ssid, try_ssid, sizeof(try_ssid));
    ip_to_str(st.sta_ip, sta_ip, sizeof(sta_ip));
    ip_to_str(st.host_ip, host_ip, sizeof(host_ip));
    ip_to_str(usb_link_host_dns(), dns, sizeof(dns));

    static const char *const try_names[] = { "idle", "running", "ok", "failed" };
    char json[800];
    snprintf(json, sizeof(json),
             "{\"wifi\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,\"reason\":%u,\"sta_ip\":\"%s\","
             "\"link\":\"%s\",\"host_ip\":\"%s\",\"dns\":\"%s\",\"usb_rx\":%lu,\"usb_tx\":%lu,\"usb_drops\":%lu,"
             "\"try_state\":\"%s\",\"try_ssid\":\"%s\"}",
             app_wifi_state_name(st.wifi), ssid, st.rssi, st.last_reason, sta_ip,
             app_link_state_name(st.link), host_ip, dns,
             (unsigned long)rx, (unsigned long)tx, (unsigned long)drops,
             try_names[st.try_state], try_ssid);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t scan_post(httpd_req_t *req)
{
    if (!mutation_allowed(req)) return deny(req);
    esp_err_t err = wifi_sta_request_scan();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
        return ESP_OK;
    }
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t scan_get(httpd_req_t *req)
{
    if (!request_allowed(req)) return deny(req);
    static wifi_ap_record_t aps[WIFI_STA_SCAN_MAX];   // 仅 httpd 单任务访问
    bool busy = false;
    size_t n = wifi_sta_scan_results(aps, WIFI_STA_SCAN_MAX, &busy);

    httpd_resp_set_type(req, "application/json");
    char item[33 * 6 + 64];
    snprintf(item, sizeof(item), "{\"busy\":%s,\"aps\":[", busy ? "true" : "false");
    httpd_resp_sendstr_chunk(req, item);
    for (size_t i = 0; i < n; i++) {
        char ssid[33 * 6 + 1];
        char raw[33] = { 0 };
        memcpy(raw, aps[i].ssid, 32);
        if (raw[0] == '\0') continue;
        json_escape(raw, ssid, sizeof(ssid));
        snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}",
                 i ? "," : "", ssid, aps[i].rssi,
                 aps[i].authmode == WIFI_AUTH_OPEN ? "true" : "false");
        httpd_resp_sendstr_chunk(req, item);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

// 有界读取整个表单体:先校验 Content-Length,循环收齐,超时/断开均视为失败。
static bool read_body(httpd_req_t *req, char *buf, size_t size)
{
    if (req->content_len == 0 || req->content_len >= size) return false;
    size_t got = 0;
    int retries = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++retries < 3) continue;
        if (r <= 0) return false;
        got += (size_t)r;
    }
    buf[got] = '\0';
    return true;
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    if (!mutation_allowed(req)) return deny(req);
    char body[FORM_BODY_MAX];
    char ssid[33], pass[65];
    if (!read_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    bool ok = form_get_field(body, "ssid", ssid, sizeof(ssid)) &&
              form_get_field(body, "pass", pass, sizeof(pass));
    memset(body, 0, sizeof(body));
    esp_err_t err = ok ? wifi_sta_request_connect(ssid, pass) : ESP_ERR_INVALID_ARG;
    memset(pass, 0, sizeof(pass));
    if (err == ESP_ERR_INVALID_ARG) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "SSID 1-32 bytes; password empty or 8-64 chars");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
        return ESP_OK;
    }
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t forget_post(httpd_req_t *req)
{
    if (!mutation_allowed(req)) return deny(req);
    if (wifi_sta_request_forget() != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
        return ESP_OK;
    }
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t log_get(httpd_req_t *req)
{
    if (!request_allowed(req)) return deny(req);
    static char buf[LOG_TOTAL];   // 仅 httpd 单任务访问
    size_t n = log_ring_read(buf, sizeof(buf));
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    for (size_t off = 0; off < n; off += LOG_CHUNK) {
        size_t len = n - off < LOG_CHUNK ? n - off : LOG_CHUNK;
        if (httpd_resp_send_chunk(req, buf + off, len) != ESP_OK) return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

esp_err_t web_cfg_start(void)
{
    if (s_server) return ESP_OK;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 3;
    cfg.backlog_conn = 2;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = 8;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务启动失败: %s", esp_err_to_name(err));
        return err;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = page_get },
        { .uri = "/api/status",  .method = HTTP_GET,  .handler = status_get },
        { .uri = "/api/scan",    .method = HTTP_GET,  .handler = scan_get },
        { .uri = "/api/scan",    .method = HTTP_POST, .handler = scan_post },
        { .uri = "/api/wifi",    .method = HTTP_POST, .handler = wifi_post },
        { .uri = "/api/forget",  .method = HTTP_POST, .handler = forget_post },
        { .uri = "/api/log",     .method = HTTP_GET,  .handler = log_get },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        err = httpd_register_uri_handler(s_server, &uris[i]);
        if (err != ESP_OK) return err;
    }
    ESP_LOGI(TAG, "配置页: http://%s/", USB_LINK_LOCAL_IP_STR);
    return ESP_OK;
}
