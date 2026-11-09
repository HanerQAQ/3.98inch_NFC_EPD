#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ISO 14443-3A & ISO-DEP (14443-4) API
bool iso14443a_poll_and_activate(uint8_t *uid, uint8_t *sak);
bool isodep_rats(uint8_t *ats, int *ats_len);
int isodep_transceive(const uint8_t *apdu_tx, uint16_t tx_len, uint8_t *apdu_rx, uint16_t max_rx, uint32_t timeout_ms);
void isodep_reset_block_number(void);
void isodep_deselect_and_halt(void);
const char* isodep_sw_string(uint16_t sw);

#ifdef __cplusplus
}
#endif
