#include "pn5180_hal.h"
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static spi_device_handle_t s_spi_dev = NULL;

static bool wait_busy_level(int level, uint32_t timeout_ms) {
    int64_t start = esp_timer_get_time();
    int64_t timeout_us = (int64_t)timeout_ms * 1000;
    while (gpio_get_level(PIN_PN5180_BUSY) != level) {
        if ((esp_timer_get_time() - start) >= timeout_us) {
            return false;
        }
        esp_rom_delay_us(10);
    }
    return true;
}

bool pn5180_command(const uint8_t *tx, uint8_t tx_len, uint8_t *rx, uint8_t rx_len) {
    if (!wait_busy_level(0, PN5180_BUSY_TIMEOUT_MS)) return false;

    gpio_set_level(PIN_PN5180_NSS, 0);
    esp_rom_delay_us(2000);

    spi_transaction_t t1;
    memset(&t1, 0, sizeof(t1));
    t1.length = tx_len * 8;
    t1.tx_buffer = tx;
    esp_err_t ret = spi_device_polling_transmit(s_spi_dev, &t1);
    if (ret != ESP_OK) {
        gpio_set_level(PIN_PN5180_NSS, 1);
        return false;
    }

    if (!wait_busy_level(1, PN5180_BUSY_TIMEOUT_MS)) {
        gpio_set_level(PIN_PN5180_NSS, 1);
        return false;
    }

    gpio_set_level(PIN_PN5180_NSS, 1);
    esp_rom_delay_us(1000);

    if (!wait_busy_level(0, PN5180_BUSY_TIMEOUT_MS)) {
        gpio_set_level(PIN_PN5180_NSS, 1);
        return false;
    }

    if (rx_len > 0 && rx != NULL) {
        gpio_set_level(PIN_PN5180_NSS, 0);
        esp_rom_delay_us(2000);

        uint8_t dummy_tx[rx_len];
        memset(dummy_tx, 0xFF, rx_len);

        spi_transaction_t t2;
        memset(&t2, 0, sizeof(t2));
        t2.length = rx_len * 8;
        t2.tx_buffer = dummy_tx;
        t2.rx_buffer = rx;
        ret = spi_device_polling_transmit(s_spi_dev, &t2);
        if (ret != ESP_OK) {
            gpio_set_level(PIN_PN5180_NSS, 1);
            return false;
        }

        if (!wait_busy_level(1, PN5180_BUSY_TIMEOUT_MS)) {
            gpio_set_level(PIN_PN5180_NSS, 1);
            return false;
        }

        gpio_set_level(PIN_PN5180_NSS, 1);
        esp_rom_delay_us(1000);

        if (!wait_busy_level(0, PN5180_BUSY_TIMEOUT_MS)) {
            return false;
        }
    }

    return true;
}

bool pn5180_read_register(uint8_t reg, uint32_t *val) {
    uint8_t cmd[2] = { CMD_READ_REGISTER, reg };
    uint8_t rx[4] = {0};
    if (!pn5180_command(cmd, 2, rx, 4)) return false;
    *val = (uint32_t)rx[0] | ((uint32_t)rx[1] << 8) | ((uint32_t)rx[2] << 16) | ((uint32_t)rx[3] << 24);
    return true;
}

bool pn5180_write_register(uint8_t reg, uint32_t val) {
    uint8_t cmd[6] = {
        CMD_WRITE_REGISTER, reg,
        (uint8_t)(val & 0xFF), (uint8_t)((val >> 8) & 0xFF),
        (uint8_t)((val >> 16) & 0xFF), (uint8_t)((val >> 24) & 0xFF)
    };
    return pn5180_command(cmd, 6, NULL, 0);
}

bool pn5180_write_register_or(uint8_t reg, uint32_t mask) {
    uint8_t cmd[6] = {
        CMD_WRITE_REGISTER_OR_MASK, reg,
        (uint8_t)(mask & 0xFF), (uint8_t)((mask >> 8) & 0xFF),
        (uint8_t)((mask >> 16) & 0xFF), (uint8_t)((mask >> 24) & 0xFF)
    };
    return pn5180_command(cmd, 6, NULL, 0);
}

bool pn5180_write_register_and(uint8_t reg, uint32_t mask) {
    uint8_t cmd[6] = {
        CMD_WRITE_REGISTER_AND_MASK, reg,
        (uint8_t)(mask & 0xFF), (uint8_t)((mask >> 8) & 0xFF),
        (uint8_t)((mask >> 16) & 0xFF), (uint8_t)((mask >> 24) & 0xFF)
    };
    return pn5180_command(cmd, 6, NULL, 0);
}

bool pn5180_read_eeprom(uint8_t addr, uint8_t *data, uint8_t len) {
    uint8_t cmd[3] = { CMD_READ_EEPROM, addr, len };
    return pn5180_command(cmd, 3, data, len);
}

bool pn5180_load_rf_config(uint8_t tx_conf, uint8_t rx_conf) {
    uint8_t cmd[3] = { CMD_LOAD_RF_CONFIG, tx_conf, rx_conf };
    return pn5180_command(cmd, 3, NULL, 0);
}

bool pn5180_rf_field_on(void) {
    const uint8_t cmd[] = { CMD_RF_ON, 0x00 };
    if (!pn5180_command(cmd, sizeof(cmd), NULL, 0)) return false;

    uint32_t irq = 0;
    int64_t start = esp_timer_get_time();
    while ((esp_timer_get_time() - start) < 1500000) {
        if (pn5180_read_register(REG_IRQ_STATUS, &irq) && (irq & IRQ_TX_RFON)) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    pn5180_write_register(REG_IRQ_CLEAR, IRQ_TX_RFON);
    return true;
}

bool pn5180_rf_field_off(void) {
    const uint8_t cmd[] = { CMD_RF_OFF, 0x00 };
    return pn5180_command(cmd, sizeof(cmd), NULL, 0);
}

uint16_t pn5180_get_agc(void) {
    uint32_t rf_stat = 0;
    if (!pn5180_read_register(REG_RF_STATUS, &rf_stat)) return 0;
    return (uint16_t)(rf_stat & 0x3FF);
}

bool pn5180_send_data(const uint8_t *data, size_t len, uint8_t valid_bits) {
    if (len > 260) return false;
    uint8_t buffer[len + 2];
    buffer[0] = CMD_SEND_DATA;
    buffer[1] = valid_bits;
    for (size_t i = 0; i < len; i++) buffer[2 + i] = data[i];

    pn5180_write_register_and(REG_SYSTEM_CONFIG, 0xFFFFFFF8);  // Idle / StopCom
    pn5180_write_register_or(REG_SYSTEM_CONFIG, 0x00000003);   // Transceive command

    return pn5180_command(buffer, len + 2, NULL, 0);
}

bool pn5180_read_data(size_t len, uint8_t *buffer) {
    uint8_t cmd[2] = { CMD_READ_DATA, 0x00 };
    return pn5180_command(cmd, sizeof(cmd), buffer, len);
}

int pn5180_transceive_rf(const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_max_len, uint32_t timeout_ms) {
    pn5180_write_register(REG_IRQ_CLEAR, 0xFFFFFFFFUL);

    if (!pn5180_send_data(tx, tx_len, 0x00)) {
        return -1;
    }

    uint32_t irq_status = 0;
    int64_t start_time = esp_timer_get_time();
    int64_t timeout_us = (int64_t)timeout_ms * 1000;
    bool rx_done = false;

    while ((esp_timer_get_time() - start_time) < timeout_us) {
        if (pn5180_read_register(REG_IRQ_STATUS, &irq_status)) {
            if (irq_status & IRQ_RX) {
                rx_done = true;
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (!rx_done) {
        return 0; // Timeout
    }

    uint32_t rx_status = 0;
    if (!pn5180_read_register(REG_RX_STATUS, &rx_status)) {
        return -1;
    }

    uint16_t len = (uint16_t)(rx_status & 0x1FF);
    if (len == 0) return 0;
    if (len > rx_max_len) len = rx_max_len;

    if (!pn5180_read_data(len, rx)) {
        return -1;
    }

    return len;
}

bool pn5180_hal_init(void) {
    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << PIN_PN5180_NSS) | (1ULL << PIN_PN5180_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&out_conf);
    gpio_set_level(PIN_PN5180_NSS, 1);
    gpio_set_level(PIN_PN5180_RST, 1);

    gpio_config_t in_conf = {
        .pin_bit_mask = (1ULL << PIN_PN5180_BUSY),
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    gpio_config(&in_conf);

    spi_bus_config_t buscfg = {
        .miso_io_num = PIN_PN5180_MISO,
        .mosi_io_num = PIN_PN5180_MOSI,
        .sclk_io_num = PIN_PN5180_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 1024,
    };
    spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 7000000,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 7,
    };
    spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi_dev);

    // Hardware Reset
    gpio_set_level(PIN_PN5180_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_PN5180_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    uint32_t irq = 0;
    int64_t start = esp_timer_get_time();
    while ((esp_timer_get_time() - start) < 1500000) {
        if (pn5180_read_register(REG_IRQ_STATUS, &irq) && (irq & IRQ_IDLE)) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    uint8_t prod_ver[2] = {0};
    pn5180_read_eeprom(0x10, prod_ver, 2);
    printf("[PN5180 HAL] Firmware Version: %u.%u\n", prod_ver[1], prod_ver[0]);

    pn5180_write_register(REG_IRQ_CLEAR, 0xFFFFFFFFUL);
    pn5180_load_rf_config(0x00, 0x80);

    return pn5180_rf_field_on();
}