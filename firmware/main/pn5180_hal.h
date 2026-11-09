#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// Hardware Pin Configuration
#define PIN_PN5180_SCK   GPIO_NUM_4
#define PIN_PN5180_MISO  GPIO_NUM_5
#define PIN_PN5180_MOSI  GPIO_NUM_6
#define PIN_PN5180_NSS   GPIO_NUM_7
#define PIN_PN5180_BUSY  GPIO_NUM_3
#define PIN_PN5180_RST   GPIO_NUM_2

// PN5180 Command Opcodes
#define CMD_WRITE_REGISTER          0x00
#define CMD_WRITE_REGISTER_OR_MASK  0x01
#define CMD_WRITE_REGISTER_AND_MASK 0x02
#define CMD_READ_REGISTER           0x04
#define CMD_READ_EEPROM             0x07
#define CMD_SEND_DATA               0x09
#define CMD_READ_DATA               0x0A
#define CMD_LOAD_RF_CONFIG          0x11
#define CMD_RF_ON                   0x16
#define CMD_RF_OFF                  0x17

// PN5180 Register Map
#define REG_SYSTEM_CONFIG           0x00
#define REG_IRQ_ENABLE              0x01
#define REG_IRQ_STATUS              0x02
#define REG_IRQ_CLEAR               0x03
#define REG_TRANSCEIVE_CONTROL      0x04
#define REG_TIMER1_RELOAD           0x0C
#define REG_TIMER1_CONFIG           0x0F
#define REG_RX_WAIT_CONFIG          0x11
#define REG_CRC_RX_CONFIG           0x12
#define REG_RX_STATUS               0x13
#define REG_CRC_TX_CONFIG           0x19
#define REG_RF_STATUS               0x1D
#define REG_SYSTEM_STATUS           0x24
#define REG_TEMP_CONTROL            0x25

// IRQ Bits
#define IRQ_RX                      (1UL << 0)
#define IRQ_TX                      (1UL << 1)
#define IRQ_IDLE                    (1UL << 2)
#define IRQ_TX_RFON                 (1UL << 9)

#define PN5180_BUSY_TIMEOUT_MS      25

// Core HAL API
bool pn5180_hal_init(void);
bool pn5180_command(const uint8_t *tx, uint8_t tx_len, uint8_t *rx, uint8_t rx_len);
bool pn5180_read_register(uint8_t reg, uint32_t *val);
bool pn5180_write_register(uint8_t reg, uint32_t val);
bool pn5180_write_register_or(uint8_t reg, uint32_t mask);
bool pn5180_write_register_and(uint8_t reg, uint32_t mask);
bool pn5180_read_eeprom(uint8_t addr, uint8_t *data, uint8_t len);
bool pn5180_load_rf_config(uint8_t tx_conf, uint8_t rx_conf);
bool pn5180_rf_field_on(void);
bool pn5180_rf_field_off(void);
uint16_t pn5180_get_agc(void);

// Data Send & Read
bool pn5180_send_data(const uint8_t *data, size_t len, uint8_t valid_bits);
bool pn5180_read_data(size_t len, uint8_t *buffer);
int pn5180_transceive_rf(const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_max_len, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
