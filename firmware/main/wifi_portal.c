#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "esp_system.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "epd_protocol.h"
#include "pn5180_hal.h"
#include "iso14443_isodep.h"
#include "epd_studio_html.h"
#include "wifi_portal.h"

static const char *TAG = "wifi_portal";

// HTTP & DNS Server Handles
static httpd_handle_t s_http_server = NULL;
static TaskHandle_t s_dns_task_handle = NULL;
static int s_dns_socket = -1;

// Dynamic refresh state
static bool s_refresh_busy = false;
static bool s_refresh_done = false;
static bool s_refresh_error = false;
static int s_progress_percent = 0;
static char s_status_msg[128] = "等待图片上传";

// Pure RAM packet stream buffer (112 KB static DRAM buffer, zero Flash erase/wear)
#define EPD_RAM_MAX_LEN     (112 * 1024)
static uint8_t s_epd_ram_buf[EPD_RAM_MAX_LEN];
static int s_packet_count = 0;
static int s_total_wire_bytes = 0;

/* ----------------- DNS 劫持 (Captive Portal) ----------------- */
static void dns_server_task(void *pvParameters)
{
    struct sockaddr_in server_addr;
    uint8_t rx_buffer[256];

    s_dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_dns_socket < 0) {
        ESP_LOGE(TAG, "Unable to create DNS socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(53);

    if (bind(s_dns_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "DNS socket bind failed: errno %d", errno);
        close(s_dns_socket);
        s_dns_socket = -1;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS Captive Portal Server listening on port 53");

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int len = recvfrom(s_dns_socket, rx_buffer, sizeof(rx_buffer), 0,
                           (struct sockaddr *)&client_addr, &client_len);
        if (len < 12 || len > 240) continue;

        // 构造 DNS 应答，将所有域名解析劫持到 192.168.4.1
        rx_buffer[2] |= 0x80; // QR=1 (Response)
        rx_buffer[3] |= 0x80; // RA=1
        rx_buffer[7] = 1;    // ANCOUNT = 1

        int idx = 12;
        while (idx < len && rx_buffer[idx] != 0) {
            idx += rx_buffer[idx] + 1;
        }
        idx += 5; // 跳过 0x00, QTYPE(2), QCLASS(2)

        if (idx + 16 <= (int)sizeof(rx_buffer)) {
            // Answer RRs
            rx_buffer[idx++] = 0xc0; // Name pointer
            rx_buffer[idx++] = 0x0c;
            rx_buffer[idx++] = 0x00; // Type A
            rx_buffer[idx++] = 0x01;
            rx_buffer[idx++] = 0x00; // Class IN
            rx_buffer[idx++] = 0x01;
            rx_buffer[idx++] = 0x00; // TTL: 60s
            rx_buffer[idx++] = 0x00;
            rx_buffer[idx++] = 0x00;
            rx_buffer[idx++] = 0x3c;
            rx_buffer[idx++] = 0x00; // Data length: 4
            rx_buffer[idx++] = 0x04;
            rx_buffer[idx++] = 192;  // 192.168.4.1
            rx_buffer[idx++] = 168;
            rx_buffer[idx++] = 4;
            rx_buffer[idx++] = 1;

            sendto(s_dns_socket, rx_buffer, idx, 0, (struct sockaddr *)&client_addr, client_len);
        }
    }
}

static void on_escort_progress(int current_sec, int total_sec, uint16_t agc, bool settling)
{
    int pct = 86 + (current_sec * 12) / (total_sec > 0 ? total_sec : EPD_ESCORT_HOLD_SECONDS);
    if (pct > 98) pct = 98;
    s_progress_percent = pct;
    if (!settling) {
        snprintf(s_status_msg, sizeof(s_status_msg),
                 "纯正弦波高压电泳无扰护航: %d/%d 秒 (AGC=%u)...", current_sec, total_sec, agc);
    } else {
        snprintf(s_status_msg, sizeof(s_status_msg),
                 "微胶囊电泳定型收尾确认中: %d/%d 秒 (AGC=%u)...", current_sec, total_sec, agc);
    }
}

/* ----------------- NFC 物理电泳单次刷屏后台任务 (纯 RAM 内存零拷贝直传) ----------------- */
static void nfc_refresh_worker_task(void *pvParameters)
{
    printf("\n==========================================================\n");
    printf("  [NFC Auto-Escort] 开始执行墨水屏单次物理刷屏护航 (RAM 内存直传 %d 分包)...\n", s_packet_count);
    printf("==========================================================\n");

    // Stage 1: Card polling
    snprintf(s_status_msg, sizeof(s_status_msg), "正在寻卡 (请确保墨水屏平放在 NFC 天线正中)...");
    s_progress_percent = 25;

    pn5180_rf_field_on();
    vTaskDelay(pdMS_TO_TICKS(150));

    uint8_t uid[4] = {0};
    uint8_t sak = 0;
    int poll_count = 0;
    bool card_found = false;

    // 寻找无源墨水屏卡片
    while (poll_count < 60) {
        uint16_t agc = pn5180_get_agc();
        if (iso14443a_poll_and_activate(uid, &sak)) {
            printf("  [OK] 成功识别被动 NFC 标签！UID: %02X %02X %02X %02X (SAK=0x%02X, AGC=%d)\n",
                   uid[0], uid[1], uid[2], uid[3], sak, agc);
            card_found = true;
            break;
        }
        poll_count++;
        if (poll_count % 4 == 0) {
            printf("  寻卡等待中... (当前 AGC=%d, 推荐贴合度 ~130-160)\n", agc);
        }
        if (poll_count % 6 == 5) {
            pn5180_rf_field_off();
            vTaskDelay(pdMS_TO_TICKS(50));
            pn5180_rf_field_on();
            vTaskDelay(pdMS_TO_TICKS(150));
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (!card_found) {
        printf("[WARN][NFC Escort] 30秒内未检测到墨水屏，已取消本次刷屏。\n");
        snprintf(s_status_msg, sizeof(s_status_msg), "寻卡超时，未检测到 3.98 寸墨水屏！");
        s_refresh_error = true;
        s_refresh_busy = false;
        pn5180_rf_field_off();
        vTaskDelete(NULL);
        return;
    }

    // Stage 2: Precharge Rail (2000ms)
    printf("\n[Stage 2/6] 纯载波预充电 3.3V 储能电容母线 (2000ms)...\n");
    snprintf(s_status_msg, sizeof(s_status_msg), "已识别标签，正在对 3.3V 储能电容预充电 (2秒)...");
    s_progress_percent = 35;
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Stage 3: ISO-DEP (RATS) + Identity + PIN Auth
    printf("\n[Stage 3/6] ISO-DEP (RATS) 握手与安全 PIN 码认证...\n");
    snprintf(s_status_msg, sizeof(s_status_msg), "建立 ISO-DEP 传输通道并校验安全 PIN 码...");
    s_progress_percent = 40;

    uint8_t ats[32];
    int ats_len = 0;
    if (!isodep_rats(ats, &ats_len)) {
        printf("[ERROR] RATS 握手失败！\n");
        snprintf(s_status_msg, sizeof(s_status_msg), "ISO-DEP (RATS) 握手失败！");
        s_refresh_error = true;
        s_refresh_busy = false;
        pn5180_rf_field_off();
        vTaskDelete(NULL);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(30));

    epd_panel_info_t info;
    if (!epd_read_identity(&info) || !epd_authenticate()) {
        printf("[ERROR] 读取屏幕参数或 PIN 认证失败！\n");
        snprintf(s_status_msg, sizeof(s_status_msg), "读取屏幕参数或 PIN 认证失败！");
        s_refresh_error = true;
        s_refresh_busy = false;
        pn5180_rf_field_off();
        vTaskDelete(NULL);
        return;
    }

    // Stage 4: Transmitting Packets directly from RAM buffer
    printf("\n[Stage 4/6] 正在传输 %d 个 RAM 内存压缩 D3 点阵包...\n", s_packet_count);
    snprintf(s_status_msg, sizeof(s_status_msg), "正在下发压缩点阵分包 (0/%d)...", s_packet_count);
    s_progress_percent = 45;

    int ram_off = 0;

    for (int i = 0; i < s_packet_count; i++) {
        if (ram_off + 7 > s_total_wire_bytes) {
            printf("\n[ERROR] RAM 分包 [%d] 偏移越界！\n", i + 1);
            snprintf(s_status_msg, sizeof(s_status_msg), "RAM 分包偏移越界！");
            s_refresh_error = true;
            s_refresh_busy = false;
            pn5180_rf_field_off();
            vTaskDelete(NULL);
            return;
        }

        const uint8_t *pkt_ptr = &s_epd_ram_buf[ram_off];
        bool is_last_in_block = (pkt_ptr[3] == 1);
        int pkt_len = 5 + pkt_ptr[4];
        if (pkt_len > 260 || pkt_len < 7 || (ram_off + pkt_len) > s_total_wire_bytes) {
            printf("\n[ERROR] 分包 [%d] 长度非法 (%d)！\n", i + 1, pkt_len);
            snprintf(s_status_msg, sizeof(s_status_msg), "分包长度非法 (%d)！", pkt_len);
            s_refresh_error = true;
            s_refresh_busy = false;
            pn5180_rf_field_off();
            vTaskDelete(NULL);
            return;
        }
        ram_off += pkt_len;

        bool pkt_success = false;
        uint32_t timeout = is_last_in_block ? 3500 : 2000;
        uint8_t resp[32];

        for (int retry = 0; retry < 8; retry++) {
            int len = isodep_transceive(pkt_ptr, pkt_len, resp, sizeof(resp), timeout);
            uint16_t sw = (len >= 2) ? (((uint16_t)resp[len - 2] << 8) | resp[len - 1]) : 0;
            if (len >= 2 && sw == 0x9000) {
                pkt_success = true;
                break;
            }
            uint16_t agc = pn5180_get_agc();
            printf("\n  [WARN][EPD RAM] Packet [%d/%d] retry %d: len=%d, SW=%04X (AGC=%d)\n",
                   i + 1, s_packet_count, retry + 1, len, sw, agc);
            vTaskDelay(pdMS_TO_TICKS(120)); // 纯载波回血储能延时
        }

        if (!pkt_success) {
            printf("\n[ERROR] 第 %d 个分包传输失败，通信异常！\n", i + 1);
            snprintf(s_status_msg, sizeof(s_status_msg), "第 %d 个分包传输失败，通信异常！", i + 1);
            s_refresh_error = true;
            s_refresh_busy = false;
            pn5180_rf_field_off();
            vTaskDelete(NULL);
            return;
        }

        if ((i + 1) % 10 == 0 || (i + 1) == s_packet_count) {
            int p = 45 + (int)((float)(i + 1) / s_packet_count * 35.0f);
            s_progress_percent = p;
            snprintf(s_status_msg, sizeof(s_status_msg), "正在传输分包: %d/%d (%d%%)...",
                     i + 1, s_packet_count, (i + 1) * 100 / s_packet_count);
            printf("[EPD Driver] Image Transfer: %3d/%d packets (%.1f%%)...\n",
                   i + 1, s_packet_count, (float)(i + 1) * 100.0f / s_packet_count);
            fflush(stdout);
        }

        if (is_last_in_block) {
            vTaskDelay(pdMS_TO_TICKS(EPD_D3_BLOCK_GAP_MS));
        } else {
            vTaskDelay(pdMS_TO_TICKS(EPD_D3_SUBPACKET_GAP_MS));
        }
    }

    // Stage 4.5: Saturate MLCC Energy Reservoir
    printf("\n[Stage 4.5] 点阵传输完成，纯载波为 MLCC 升压电容充能 (2000ms)...\n");
    snprintf(s_status_msg, sizeof(s_status_msg), "数据传输完成，为高压升压泵蓄能 (2000ms)...");
    s_progress_percent = 82;
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Stage 5: Trigger Refresh (D4)
    printf("\n[Stage 5/6] 触发高压电泳软启动指令 (D4: 2000ms)...\n");
    snprintf(s_status_msg, sizeof(s_status_msg), "正在触发高压电泳刷新 (D4 软启动)...");
    s_progress_percent = 85;
    if (!epd_trigger_refresh(2000, false)) {
        printf("[ERROR] 触发刷新指令响应失败！\n");
        snprintf(s_status_msg, sizeof(s_status_msg), "触发刷新指令响应失败！");
        s_refresh_error = true;
        s_refresh_busy = false;
        pn5180_rf_field_off();
        vTaskDelete(NULL);
        return;
    }

    // Stage 6: Electrophoretic Escort
    printf("\n[Stage 6/6] 正在执行 %d 秒高压电泳满血护航 (0~%ds 纯正弦波零调制，零查询)...\n",
           EPD_ESCORT_HOLD_SECONDS, EPD_ESCORT_HOLD_SECONDS);
    snprintf(s_status_msg, sizeof(s_status_msg), "正在进行纯正弦波高压电泳无扰护航 (微胶囊电泳迁移中)...");
    s_progress_percent = 86;

    epd_hold_and_keepalive_cb(EPD_ESCORT_HOLD_SECONDS, on_escort_progress);

    // 4-Step Safe Shutdown & RF Cut Off
    snprintf(s_status_msg, sizeof(s_status_msg), "正在执行 4 步高压电荷泵安全泄放与关场...");
    s_progress_percent = 99;
    epd_safe_shutdown();

    s_progress_percent = 100;
    s_refresh_done = true;
    s_refresh_busy = false;
    snprintf(s_status_msg, sizeof(s_status_msg), "[完成] 墨水屏刷新大功告成！画面已固化，射频天线场已安全断电。");
    printf("\n==========================================================\n");
    printf("  [SUCCESS] 墨水屏物理刷新完成！画面已固化，射频天线场已断电！\n");
    printf("==========================================================\n\n");
    vTaskDelete(NULL);
}

/* ----------------- HTTP 请求路由处理 ----------------- */

// GET /favicon.ico -> 204 No Content
static esp_err_t favicon_get_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// GET / -> 返回内置 Gzip 压缩的高级 Web 刷图工作台
static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    httpd_resp_send(req, (const char *)EPD_STUDIO_HTML_GZ, EPD_STUDIO_HTML_GZ_LEN);
    return ESP_OK;
}

// GET /api/status -> 轮询当前墨水屏刷新进度与状态
static esp_err_t status_get_handler(httpd_req_t *req)
{
    char json_buf[256];
    snprintf(json_buf, sizeof(json_buf),
             "{\"busy\":%s,\"percent\":%d,\"status\":\"%s\",\"done\":%s,\"error\":%s}",
             s_refresh_busy ? "true" : "false",
             s_progress_percent,
             s_status_msg,
             s_refresh_done ? "true" : "false",
             s_refresh_error ? "true" : "false");

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, json_buf);
    return ESP_OK;
}

// POST /upload_photo -> 流式接收数据直接写入 RAM 内存缓冲区 (0 Flash 擦写、0 磨损、毫秒级响应)
static esp_err_t upload_post_handler(httpd_req_t *req)
{
    int total_len = req->content_len;
    ESP_LOGI(TAG, "Incoming upload stream (RAM direct): content_len = %d", total_len);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (total_len <= 0 || total_len > EPD_RAM_MAX_LEN) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"无效的数据包长度\"}");
        return ESP_OK;
    }

    if (s_refresh_busy) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"墨水屏当前正在物理刷新中，请稍候！\"}");
        return ESP_OK;
    }

    // 1. 接收数据直接存入 112KB 动态 DRAM 静态内存缓冲区
    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, (char *)&s_epd_ram_buf[received], total_len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "HTTP receive failed: %d", ret);
            httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"接收图片二进制流失败\"}");
            return ESP_FAIL;
        }
        received += ret;
    }

    // 2. 遍历校验并统计分包数量
    int pkt_count = 0;
    int off = 0;
    while (off + 7 <= total_len) {
        if (s_epd_ram_buf[off] != 0xF0 || s_epd_ram_buf[off + 1] != 0xD3) {
            ESP_LOGW(TAG, "Invalid D3 header at offset %d", off);
            break;
        }
        uint8_t len_plus_2 = s_epd_ram_buf[off + 4];
        int pkt_len = 5 + len_plus_2;
        if (pkt_len < 7 || pkt_len > 260 || (off + pkt_len) > total_len) break;
        pkt_count++;
        off += pkt_len;
    }

    if (pkt_count == 0) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"数据中未找到有效的 D3 压缩分包\"}");
        return ESP_OK;
    }

    s_total_wire_bytes = off;
    s_packet_count = pkt_count;
    s_refresh_busy = true;
    s_refresh_done = false;
    s_refresh_error = false;
    s_progress_percent = 20;
    snprintf(s_status_msg, sizeof(s_status_msg), "已接收 %d 个分包 (RAM 内存直传)，准备刷屏...", pkt_count);

    // 3. 启动异步电泳护航任务
    xTaskCreate(nfc_refresh_worker_task, "nfc_ram_escort", 4096, NULL, 5, NULL);

    char resp_buf[128];
    snprintf(resp_buf, sizeof(resp_buf),
             "{\"ok\":true,\"packets\":%d,\"bytes\":%d,\"msg\":\"上传成功，开始刷屏\"}",
             pkt_count, off);
    httpd_resp_sendstr(req, resp_buf);
    return ESP_OK;
}

// 供串口 'u' 指令直接向 RAM 缓冲区加载数据并触发刷新
uint8_t *wifi_portal_get_ram_buf(void)
{
    return s_epd_ram_buf;
}

bool wifi_portal_start_ram_refresh(int total_len)
{
    if (s_refresh_busy || total_len <= 0 || total_len > EPD_RAM_MAX_LEN) {
        return false;
    }

    int pkt_count = 0;
    int off = 0;
    while (off + 7 <= total_len) {
        if (s_epd_ram_buf[off] != 0xF0 || s_epd_ram_buf[off + 1] != 0xD3) break;
        uint8_t len_plus_2 = s_epd_ram_buf[off + 4];
        int pkt_len = 5 + len_plus_2;
        if (pkt_len < 7 || pkt_len > 260 || (off + pkt_len) > total_len) break;
        pkt_count++;
        off += pkt_len;
    }

    if (pkt_count == 0) {
        return false;
    }

    s_total_wire_bytes = off;
    s_packet_count = pkt_count;
    s_refresh_busy = true;
    s_refresh_done = false;
    s_refresh_error = false;
    s_progress_percent = 20;
    snprintf(s_status_msg, sizeof(s_status_msg), "已接收 %d 个分包 (RAM 内存直传)，准备刷屏...", pkt_count);

    xTaskCreate(nfc_refresh_worker_task, "nfc_ram_escort", 4096, NULL, 5, NULL);
    return true;
}

// 触发 RAM 缓冲区中已上传图像的再次刷入 (供串口 'r' 快捷重刷使用)
bool wifi_portal_trigger_ram_refresh(void)
{
    if (s_refresh_busy || s_packet_count <= 0 || s_total_wire_bytes <= 0) {
        return false;
    }

    s_refresh_busy = true;
    s_refresh_done = false;
    s_refresh_error = false;
    s_progress_percent = 20;
    snprintf(s_status_msg, sizeof(s_status_msg), "正在重新触发 RAM 图像刷新 (%d 分包)...", s_packet_count);

    xTaskCreate(nfc_refresh_worker_task, "nfc_ram_escort", 4096, NULL, 5, NULL);
    return true;
}

/* ----------------- Web 服务器初始化与启动 ----------------- */
esp_err_t wifi_portal_start(void)
{
    // 1. 初始化 TCP/IP 适配层
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();

    // 配置 AP 的静态网关 IP: 192.168.4.1
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(ap_netif);
    esp_netif_set_ip_info(ap_netif, &ip_info);
    esp_netif_dhcps_start(ap_netif);

    // 2. 配置并启动 Wi-Fi SoftAP
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_PORTAL_SSID,
            .ssid_len = strlen(WIFI_PORTAL_SSID),
            .channel = 1,
            .password = "",
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi SoftAP started. SSID: %s, IP: %s", WIFI_PORTAL_SSID, WIFI_PORTAL_IP);

    // 3. 启动 DNS 劫持后台服务
    xTaskCreate(dns_server_task, "dns_server", 2560, NULL, 5, &s_dns_task_handle);

    // 4. 启动 HTTP 嵌入式 Web 工作台服务
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.max_uri_handlers = 8;
    http_cfg.stack_size = 8192;
    http_cfg.lru_purge_enable = true;

    if (httpd_start(&s_http_server, &http_cfg) == ESP_OK) {
        httpd_uri_t favicon_uri = {
            .uri = "/favicon.ico",
            .method = HTTP_GET,
            .handler = favicon_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_http_server, &favicon_uri);

        httpd_uri_t index_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = index_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_http_server, &index_uri);

        httpd_uri_t status_uri = {
            .uri = "/api/status",
            .method = HTTP_GET,
            .handler = status_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_http_server, &status_uri);

        httpd_uri_t upload_uri = {
            .uri = "/upload_photo",
            .method = HTTP_POST,
            .handler = upload_post_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_http_server, &upload_uri);

        ESP_LOGI(TAG, "HTTP Server started on http://%s/", WIFI_PORTAL_IP);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to start HTTP server!");
    return ESP_FAIL;
}

void wifi_portal_stop(void)
{
    if (s_http_server) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
    }
    if (s_dns_socket >= 0) {
        close(s_dns_socket);
        s_dns_socket = -1;
    }
    if (s_dns_task_handle) {
        vTaskDelete(s_dns_task_handle);
        s_dns_task_handle = NULL;
    }
    esp_wifi_stop();
}

bool wifi_portal_is_busy(void)
{
    return s_refresh_busy;
}
