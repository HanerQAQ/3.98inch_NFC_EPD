#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_PORTAL_SSID "EPD-3.98-NFC"
#define WIFI_PORTAL_IP   "192.168.4.1"

// 112KB 动态 DRAM 静态内存缓冲区大小
#define EPD_RAM_MAX_LEN  (112 * 1024)

esp_err_t wifi_portal_start(void);
void wifi_portal_stop(void);
bool wifi_portal_is_busy(void);
uint8_t *wifi_portal_get_ram_buf(void);
bool wifi_portal_start_ram_refresh(int total_len);
bool wifi_portal_trigger_ram_refresh(void);

#ifdef __cplusplus
}
#endif
