#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_EPD_PACKETS 480

// D3 pacing: qpoll-lite sends packets back-to-back, gated by the panel's ISO-DEP
// response. Keep a small carrier-recharge gap for PN5180 field-powered panels.
#define EPD_D3_SUBPACKET_GAP_MS 8
#define EPD_D3_BLOCK_GAP_MS     20

// D4 escort: pure-carrier hold after the refresh command.
#define EPD_ESCORT_HOLD_SECONDS          22
#define EPD_ESCORT_PURE_CARRIER_SECONDS  35

typedef struct {
    uint8_t block;
    uint8_t subpacket;
    bool last_in_block;
    uint16_t len;
    uint8_t inf[260];
} epd_packet_t;

typedef struct {
    uint8_t uid[8];
    uint8_t a0_geom[7];
    uint8_t a1_scan[7];
    bool top_down;
} epd_panel_info_t;

// Optional progress callback during electrophoretic refresh escort
typedef void (*epd_escort_cb_t)(int current_sec, int total_sec, uint16_t agc, bool settling);

// EPD Protocol APIs (Ported & refined from QPoll Lite PanelProtocol.java)
bool epd_read_identity(epd_panel_info_t *info);
bool epd_authenticate(void);
bool epd_transfer_packets(const epd_packet_t *packets, int count);
bool epd_trigger_refresh(uint16_t power_ms, bool official_check);
bool epd_hold_and_keepalive(int hold_seconds);
bool epd_hold_and_keepalive_cb(int hold_seconds, epd_escort_cb_t cb);
void epd_safe_shutdown(void);

#ifdef __cplusplus
}
#endif
