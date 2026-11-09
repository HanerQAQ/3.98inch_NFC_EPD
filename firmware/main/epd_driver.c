#include "epd_protocol.h"
#include "iso14443_isodep.h"
#include "pn5180_hal.h"
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const uint8_t CMD_D1[]  = { 0x00, 0xD1, 0x00, 0x00, 0x00 };
static const uint8_t CMD_PIN[] = { 0x00, 0x20, 0x00, 0x01, 0x05, 0x52, 0x65, 0x69, 0x99, 0x19 };
static const uint8_t CMD_D8[]  = { 0xF0, 0xD8, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x0E };
static const uint8_t CMD_DE[]  = { 0xF0, 0xDE, 0x00, 0x00, 0x01 };

bool epd_read_identity(epd_panel_info_t *info) {
    uint8_t resp[128];
    int len = isodep_transceive(CMD_D1, sizeof(CMD_D1), resp, sizeof(resp), 2000);
    if (len < 2) {
        printf("[EPD Driver] D1 read failed (len=%d)\n", len);
        return false;
    }
    uint16_t sw = ((uint16_t)resp[len - 2] << 8) | resp[len - 1];
    if (sw != 0x9000) {
        printf("[EPD Driver] D1 rejected with SW=%04X (%s)\n", sw, isodep_sw_string(sw));
        return false;
    }

    // Parse TLV tags
    int offset = 0;
    int payload_len = len - 2;
    memset(info, 0, sizeof(*info));
    info->top_down = true; // Default

    while (offset + 2 <= payload_len) {
        uint8_t tag = resp[offset++];
        uint8_t tag_len = resp[offset++];
        if (offset + tag_len > payload_len) break;

        if (tag == 0xA0 && tag_len <= 7) {
            memcpy(info->a0_geom, &resp[offset], tag_len);
        } else if (tag == 0xA1 && tag_len <= 7) {
            memcpy(info->a1_scan, &resp[offset], tag_len);
            info->top_down = (resp[offset] == 1);
        } else if (tag == 0xC1 && tag_len <= 8) {
            memcpy(info->uid, &resp[offset], tag_len);
        }
        offset += tag_len;
    }

    printf("[EPD Driver] D1 Identity: Scan Direction = %s (0x%02X)\n",
           info->top_down ? "TopDown (01)" : "BottomUp (00)",
           info->a1_scan[0]);
    return true;
}

bool epd_authenticate(void) {
    uint8_t resp[64];
    int len = isodep_transceive(CMD_D8, sizeof(CMD_D8), resp, sizeof(resp), 1500);
    uint16_t sw = (len >= 2) ? (((uint16_t)resp[len - 2] << 8) | resp[len - 1]) : 0;

    if (sw == 0x6985) {
        printf("[EPD Driver] D8 reported 6985 (PIN required), validating standard PIN...\n");
        // Verify PIN
        len = isodep_transceive(CMD_PIN, sizeof(CMD_PIN), resp, sizeof(resp), 1500);
        sw = (len >= 2) ? (((uint16_t)resp[len - 2] << 8) | resp[len - 1]) : 0;
        if (sw != 0x9000) {
            printf("[EPD Driver] PIN verification failed: SW=%04X (%s)\n", sw, isodep_sw_string(sw));
            return false;
        }
        printf("[EPD Driver] PIN verified successfully (9000)\n");

        // Re-check D8
        len = isodep_transceive(CMD_D8, sizeof(CMD_D8), resp, sizeof(resp), 1500);
        sw = (len >= 2) ? (((uint16_t)resp[len - 2] << 8) | resp[len - 1]) : 0;
    }

    if (sw != 0x9000) {
        printf("[EPD Driver] D8 permission check failed: SW=%04X (%s)\n", sw, isodep_sw_string(sw));
        return false;
    }

    printf("[EPD Driver] EPD Security Access Granted (9000)\n");
    return true;
}

bool epd_transfer_packets(const epd_packet_t *packets, int count) {
    uint8_t resp[32];
    int64_t t_start = esp_timer_get_time();

    for (int i = 0; i < count; i++) {
        const epd_packet_t *pkt = &packets[i];
        bool pkt_success = false;
        uint32_t d3_timeout = pkt->last_in_block ? 3500 : 2000;

        for (int retry = 0; retry < 5; retry++) {
            int len = isodep_transceive(pkt->inf, pkt->len, resp, sizeof(resp), d3_timeout);
            uint16_t sw = (len >= 2) ? (((uint16_t)resp[len - 2] << 8) | resp[len - 1]) : 0;
            if (len >= 2 && sw == 0x9000) {
                pkt_success = true;
                break;
            }
            uint16_t agc = pn5180_get_agc();
            printf("\n  [WARN][EPD Driver] Packet [%d/%d] retry %d: len=%d, SW=%04X (AGC=%d)\n",
                   i + 1, count, retry + 1, len, sw, agc);
            vTaskDelay(pdMS_TO_TICKS(100)); // 纯正弦波回血储能延时
        }

        if (!pkt_success) {
            printf("\n[EPD Driver] D3 Packet %d/%d communication failed after retries!\n", i + 1, count);
            return false;
        }

        if ((i + 1) % 10 == 0 || (i + 1) == count) {
            float progress = (float)(i + 1) / count * 100.0f;
            printf("\r[EPD Driver] Image Transfer: %3d/%d packets (%.1f%%)...", i + 1, count, progress);
            fflush(stdout);
        }

        if (pkt->last_in_block) {
            vTaskDelay(pdMS_TO_TICKS(EPD_D3_BLOCK_GAP_MS));
        } else {
            vTaskDelay(pdMS_TO_TICKS(EPD_D3_SUBPACKET_GAP_MS));
        }
    }

    int64_t elapsed_ms = (esp_timer_get_time() - t_start) / 1000;
    printf("\n[EPD Driver] Image Transmission Completed: %d packets in %lld ms (avg %.1f ms/pkt)\n",
           count, elapsed_ms, (float)elapsed_ms / count);
    return true;
}

bool epd_trigger_refresh(uint16_t power_ms, bool official_check) {
    // D4 Format: F0 D4 <0x80 | (power_ms/100)> <official_check ? 0x00 : 0x80> 00
    uint8_t d4_cmd[5] = {
        0xF0, 0xD4,
        (uint8_t)(0x80 | (power_ms / 100)),
        (uint8_t)(official_check ? 0x00 : 0x80),
        0x00
    };

    printf("[EPD Driver] Sending D4 Refresh Command (PowerMs=%d, Mode=%s)...\n",
           power_ms, official_check ? "Official Check (0x00)" : "Fast Mode (0x80)");

    uint8_t resp[32];
    uint32_t timeout = official_check ? 30000 : 5000;
    int attempt = 0;
    uint16_t final_sw = 0;

    while (attempt < 3) {
        attempt++;
        int len = isodep_transceive(d4_cmd, sizeof(d4_cmd), resp, sizeof(resp), timeout);
        if (len < 2) {
            printf("[EPD Driver] D4 transceive failed on attempt %d\n", attempt);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        final_sw = ((uint16_t)resp[len - 2] << 8) | resp[len - 1];
        if (final_sw == 0x9000) {
            break;
        }
        if (final_sw == 0x6986) {
            printf("[EPD Driver] Screen reported 6986 (Busy refreshing), retrying D4 (%d/3)...\n", attempt);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        break;
    }

    if (final_sw != 0x9000) {
        printf("[EPD Driver] D4 Refresh start failed: SW=%04X (%s)\n", final_sw, isodep_sw_string(final_sw));
        return false;
    }

    printf("[EPD Driver] D4 Refresh Command Accepted (SW=9000)!\n");
    return true;
}

bool epd_hold_and_keepalive_cb(int hold_seconds, epd_escort_cb_t cb) {
    printf("[EPD Driver] Escorting Refresh: Full-lifecycle RF hold (up to %d seconds)...\n", hold_seconds);
    int pure_sec = (hold_seconds < EPD_ESCORT_PURE_CARRIER_SECONDS) ? hold_seconds : EPD_ESCORT_PURE_CARRIER_SECONDS;
    printf("  [Phase 1] 0~%ds: 100%% Pure Sine Wave (Zero-ASK, Charge Pump Protection)\n", pure_sec);
    if (hold_seconds > EPD_ESCORT_PURE_CARRIER_SECONDS) {
        printf("  [Phase 2] %d~%ds: Periodic Closed-Loop Verification (CMD_DE Query)\n",
               EPD_ESCORT_PURE_CARRIER_SECONDS + 1, hold_seconds);
    }

    uint8_t resp[16];
    bool idle_confirmed = false;
    int consecutive_lost = 0;

    for (int sec = 1; sec <= hold_seconds; sec++) {
        uint16_t agc = pn5180_get_agc();

        if (sec <= EPD_ESCORT_PURE_CARRIER_SECONDS) {
            printf("\r[EPD Driver] High-Voltage Pure Sine Wave: sec %2d/%d (Phase 1: Zero-ASK Pure Carrier, AGC=%d)...",
                   sec, hold_seconds, agc);
            fflush(stdout);
            if (cb) cb(sec, hold_seconds, agc, false);
            vTaskDelay(pdMS_TO_TICKS(1000));
        } else {
            if (cb) cb(sec, hold_seconds, agc, true);
            if (sec % 2 == 0) {
                int len = isodep_transceive(CMD_DE, sizeof(CMD_DE), resp, sizeof(resp), 1500);
                if (len >= 2) {
                    consecutive_lost = 0;
                    uint16_t sw = ((uint16_t)resp[len - 2] << 8) | resp[len - 1];
                    if (sw == 0x9000 && len >= 3) {
                        uint8_t status = resp[0];
                        if (status == 0x00) {
                            printf("\n[EPD Driver] Electrophoretic Waveform Completed at sec %d! status=0x00 (IDLE, SW=9000, AGC=%d)\n",
                                   sec, agc);
                            idle_confirmed = true;
                            break;
                        } else {
                            printf("\r[EPD Driver] Electrophoretic Settling: sec %2d/%d (status=0x%02X, SW=9000, AGC=%d)...",
                                   sec, hold_seconds, status, agc);
                            fflush(stdout);
                        }
                    } else {
                        printf("\r[EPD Driver] Electrophoretic Settling: sec %2d/%d (SW=%04X, AGC=%d)...",
                               sec, hold_seconds, sw, agc);
                        fflush(stdout);
                    }
                } else {
                    consecutive_lost++;
                    printf("\r[EPD Driver] Electrophoretic Settling: sec %2d/%d (AGC=%d, DE drop %d/5)...",
                           sec, hold_seconds, agc, consecutive_lost);
                    fflush(stdout);
                    if (consecutive_lost >= 5) {
                        printf("\n[EPD Driver] RF Carrier Lost: 5 consecutive drops. Screen may be removed!\n");
                        return false;
                    }
                }
            } else {
                printf("\r[EPD Driver] Electrophoretic Settling: sec %2d/%d (Pure Carrier Inter-Slot, AGC=%d)...",
                       sec, hold_seconds, agc);
                fflush(stdout);
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    if (!idle_confirmed) {
        printf("\n[EPD Driver] Electrophoretic Waveform Escort Finished (%d seconds elapsed).\n", hold_seconds);
    }
    return true;
}

bool epd_hold_and_keepalive(int hold_seconds) {
    return epd_hold_and_keepalive_cb(hold_seconds, NULL);
}

void epd_safe_shutdown(void) {
    printf("[EPD Driver] Executing 4-Step Safe Power-Down Sequence...\n");

    // Step 1: Idle DE confirmation
    uint8_t resp[16];
    isodep_transceive(CMD_DE, sizeof(CMD_DE), resp, sizeof(resp), 500);

    // Step 2: 4000ms Carrier hold for MLCC / Charge pump discharge (+-15V -> 0V)
    printf("  [Step 2/4] Discharging VGH/VGL (±15V) high voltage rails (4 seconds)...\n");
    vTaskDelay(pdMS_TO_TICKS(4000));

    // Step 3: ISO-DEP S(DESELECT) and HLTA
    printf("  [Step 3/4] Sending ISO-DEP S(DESELECT) & HLTA...\n");
    isodep_deselect_and_halt();

    // Step 4: Turn off RF Carrier
    printf("  [Step 4/4] Shutting down 13.56 MHz RF Field (CMD_RF_OFF)...\n");
    pn5180_rf_field_off();
    printf("[EPD Driver] Safe Power-Down Sequence Finished!\n");
}