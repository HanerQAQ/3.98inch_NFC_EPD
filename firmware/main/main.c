#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "pn5180_hal.h"
#include "iso14443_isodep.h"
#include "epd_protocol.h"
#include "wifi_portal.h"

static const char *TAG = "main";

static inline int hex_nibble(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void handle_serial_ram_upload(void) {
    if (wifi_portal_is_busy()) {
        printf("ERR_BUSY\n");
        fflush(stdout);
        return;
    }

    printf("READY_RAM_HEX\n");
    fflush(stdout);

    // 1. Read total byte length line
    char len_buf[24] = {0};
    int len_idx = 0;
    int64_t t0 = esp_timer_get_time();
    while ((esp_timer_get_time() - t0) < 5000000LL) {
        int ch = fgetc(stdin);
        if (ch < 0) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        if (ch == '\n' || ch == '\r') {
            if (len_idx > 0) break;
            continue;
        }
        if (ch >= '0' && ch <= '9' && len_idx < (int)sizeof(len_buf) - 1) {
            len_buf[len_idx++] = (char)ch;
        }
    }

    int total_len = atoi(len_buf);
    if (total_len <= 0 || total_len > EPD_RAM_MAX_LEN) {
        printf("ERR_LEN:%d\n", total_len);
        fflush(stdout);
        return;
    }

    printf("ACK_LEN:%d\n", total_len);
    fflush(stdout);

    // 2. Receive hex-encoded D3 stream directly into static RAM buffer
    uint8_t *ram_buf = wifi_portal_get_ram_buf();
    int received = 0;
    int high_nib = -1;
    int64_t last_rx = esp_timer_get_time();

    while (received < total_len) {
        int ch = fgetc(stdin);
        if (ch < 0) {
            if ((esp_timer_get_time() - last_rx) > 5000000LL) {
                printf("ERR_TIMEOUT:%d/%d\n", received, total_len);
                fflush(stdout);
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        last_rx = esp_timer_get_time();
        int nib = hex_nibble(ch);
        if (nib < 0) continue;

        if (high_nib < 0) {
            high_nib = nib;
        } else {
            ram_buf[received++] = (uint8_t)((high_nib << 4) | nib);
            high_nib = -1;
        }
    }

    if (wifi_portal_start_ram_refresh(total_len)) {
        printf("RAM_LOAD_OK:%d\n", total_len);
    } else {
        printf("ERR_INVALID_D3\n");
    }
    fflush(stdout);
}

void app_main(void) {
    vTaskDelay(pdMS_TO_TICKS(300));

    printf("\n==========================================================\n");
    printf("  3.98\" Passive NFC EPD Studio (Pure RAM Direct Mode)    \n");
    printf("  Direct Client-Side Dither & Compression (Zero Flash Wear)\n");
    printf("==========================================================\n\n");

    // 1. 初始化 NVS 存储 (供 Wi-Fi 射频校准使用)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. 初始化 PN5180 射频硬件并默认关闭天线场 (杜绝开机误刷与发热)
    if (!pn5180_hal_init()) {
        ESP_LOGE(TAG, "PN5180 Hardware Initialization Failed!");
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    pn5180_rf_field_off();
    ESP_LOGI(TAG, "PN5180 HAL initialized, RF field is OFF.");

    // 3. 启动独立 Wi-Fi SoftAP 与嵌入式 Web 工作台
    wifi_portal_start();

    printf("\n==========================================================\n");
    printf("  [READY] 设备已就绪！您可以通过以下方式使用：\n");
    printf("  【无线直传】连接 Wi-Fi: %s (开放无密码)\n", WIFI_PORTAL_SSID);
    printf("             手机/电脑访问: http://%s/\n", WIFI_PORTAL_IP);
    printf("  【串口指令】输入 'r' 重新刷入当前 RAM 内存中的缓存图像\n");
    printf("             输入 'u' 启动串口十六进制流直传加载 RAM\n");
    printf("==========================================================\n\n");

    // 4. 主任务监听串口指令 (非阻塞轮询)
    while (1) {
        int c = fgetc(stdin);
        if (c == 'r' || c == 'R') {
            if (!wifi_portal_trigger_ram_refresh()) {
                printf("[WARN][NFC RAM] 当前 RAM 缓冲区尚无上传图像或正在刷屏中，请先通过网页上传图片！\n");
            }
        } else if (c == 'u' || c == 'U') {
            handle_serial_ram_upload();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
