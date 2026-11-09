#include "iso14443_isodep.h"
#include "pn5180_hal.h"
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static uint8_t s_block_number = 0;
#define MAX_TX_INF 240

void isodep_reset_block_number(void) {
    s_block_number = 0;
}

const char* isodep_sw_string(uint16_t sw) {
    switch (sw) {
        case 0x9000: return "OK (Success)";
        case 0x6502: case 0x68CA: case 0x698A: return "Low Voltage / Brownout (68CA)";
        case 0x68CB: return "Refresh Timeout";
        case 0x6992: return "LZO Decompress Failed";
        case 0x6993: return "Decompressed Length Error";
        case 0x6995: return "Packet Sequence Error";
        case 0x6982: return "Security Status Not Satisfied";
        case 0x6985: return "Access Condition Not Satisfied (PIN needed)";
        case 0x6986: case 0x68C6: return "Screen Busy Refreshing";
        default: return "Device Error";
    }
}

bool iso14443a_poll_and_activate(uint8_t *uid, uint8_t *sak) {
    uint8_t buffer[10] = {0};
    uint8_t cmd[10] = {0};

    pn5180_write_register_and(REG_SYSTEM_CONFIG, 0xFFFFFFF8);
    if (!pn5180_load_rf_config(0x00, 0x80)) return false;
    if (!pn5180_write_register_and(REG_SYSTEM_CONFIG, 0xFFFFFFBF)) return false;
    if (!pn5180_write_register_and(REG_CRC_RX_CONFIG, 0xFFFFFFFE)) return false;
    if (!pn5180_write_register_and(REG_CRC_TX_CONFIG, 0xFFFFFFFE)) return false;

    // 1. 发 7-bit 0x7A 专用唤醒短帧
    cmd[0] = 0x7A;
    if (!pn5180_send_data(cmd, 1, 0x07)) return false;

    if (!pn5180_read_data(2, buffer)) return false;
    if (buffer[0] != 0x04 || buffer[1] != 0x00) {
        // 备选 0x52 (WUPA)
        cmd[0] = 0x52;
        if (!pn5180_send_data(cmd, 1, 0x07)) return false;
        if (!pn5180_read_data(2, buffer)) return false;
        if (buffer[0] != 0x04 || buffer[1] != 0x00) return false;
    }

    // 2. 发防冲突 CL1 (0x93 0x20，无 CRC)
    cmd[0] = 0x93;
    cmd[1] = 0x20;
    if (!pn5180_send_data(cmd, 2, 0x00)) return false;
    if (!pn5180_read_data(5, cmd + 2)) return false;

    // 校验 BCC
    if ((cmd[2] ^ cmd[3] ^ cmd[4] ^ cmd[5]) != cmd[6]) {
        return false;
    }

    // 3. 开启硬件 CRC 校验 (SELECT 帧带 CRC)
    if (!pn5180_write_register_or(REG_CRC_RX_CONFIG, 0x01)) return false;
    if (!pn5180_write_register_or(REG_CRC_TX_CONFIG, 0x01)) return false;

    // 4. 发 Select CL1 (0x93 0x70 + 4 字节 UID + 1 字节 BCC)
    cmd[0] = 0x93;
    cmd[1] = 0x70;
    if (!pn5180_send_data(cmd, 7, 0x00)) return false;
    if (!pn5180_read_data(1, buffer + 2)) return false;

    *sak = buffer[2];
    for (int i = 0; i < 4; i++) {
        uid[i] = cmd[2 + i];
    }

    return true;
}

bool isodep_rats(uint8_t *ats, int *ats_len) {
    isodep_reset_block_number();
    const uint8_t rats_cmd[] = { 0xE0, 0x80 }; // FSDI=8 (256 bytes), CID=0
    uint8_t buf[32];
    int len = pn5180_transceive_rf(rats_cmd, sizeof(rats_cmd), buf, sizeof(buf), 200);
    if (len <= 0) return false;

    if (ats && ats_len) {
        memcpy(ats, buf, len);
        *ats_len = len;
    }
    return true;
}

int isodep_transceive(const uint8_t *apdu, uint16_t apdu_len, uint8_t *resp, uint16_t resp_max_len, uint32_t timeout_ms) {
    size_t offset = 0;
    while (offset < apdu_len || apdu_len == 0) {
        size_t chunk_len = apdu_len - offset;
        if (chunk_len > MAX_TX_INF) {
            chunk_len = MAX_TX_INF;
        }
        bool more = (offset + chunk_len) < apdu_len;

        uint8_t tx_frame[260];
        uint8_t pcb = 0x02 | (s_block_number & 0x01);
        if (more) {
            pcb |= 0x10;
        }
        tx_frame[0] = pcb;
        if (chunk_len > 0) {
            memcpy(&tx_frame[1], &apdu[offset], chunk_len);
        }
        size_t tx_len = 1 + chunk_len;

        uint8_t orig_tx_frame[260];
        memcpy(orig_tx_frame, tx_frame, tx_len);
        size_t orig_tx_len = tx_len;

        int wtx_count = 0;
        bool chunk_done = false;
        int chunk_retries = 0;
        const int max_chunk_retries = 10;

        while (!chunk_done) {
            uint8_t rx_frame[260];
            int rx_len = pn5180_transceive_rf(tx_frame, tx_len, rx_frame, sizeof(rx_frame), timeout_ms);
            if (rx_len <= 0) {
                chunk_retries++;
                if (chunk_retries > max_chunk_retries) {
                    return -1;
                }
                // 恢复原始 I-block 帧，防止被中间 WTX 响应覆盖
                memcpy(tx_frame, orig_tx_frame, orig_tx_len);
                tx_len = orig_tx_len;
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }

            // S(WTX)
            if ((rx_frame[0] & 0xF7) == 0xF2) {
                wtx_count++;
                if (wtx_count > 80) return -2;
                uint8_t wtxm = rx_frame[1] & 0x3F;
                uint8_t wtx_resp[2] = { 0xF2, wtxm };
                tx_frame[0] = wtx_resp[0];
                tx_frame[1] = wtx_resp[1];
                tx_len = 2;
                continue;
            }

            // R(ACK) Chaining ack
            if (more) {
                if ((rx_frame[0] & 0xE6) == 0xA2) {
                    s_block_number ^= 1;
                    offset += chunk_len;
                    chunk_done = true;
                    break;
                }
                chunk_retries++;
                if (chunk_retries > max_chunk_retries) return -3;
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }

            // I-block Response
            if ((rx_frame[0] & 0xC2) == 0x02) {
                size_t payload_len = rx_len - 1;
                if (payload_len > resp_max_len) payload_len = resp_max_len;
                memcpy(resp, &rx_frame[1], payload_len);
                s_block_number = (rx_frame[0] & 0x01) ^ 1;
                return (int)payload_len;
            }

            return -4;
        }

        if (apdu_len == 0) break;
    }

    return 0;
}

void isodep_deselect_and_halt(void) {
    const uint8_t deselect_cmd[] = { 0xC2 };
    uint8_t resp[16];
    pn5180_transceive_rf(deselect_cmd, sizeof(deselect_cmd), resp, sizeof(resp), 100);

    const uint8_t hlta_cmd[] = { 0x50, 0x00 };
    pn5180_transceive_rf(hlta_cmd, sizeof(hlta_cmd), resp, sizeof(resp), 100);
}