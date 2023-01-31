/*
 bmp180.h - BMP180 pressure sensor I2C driver for ESP32
*/

#ifndef BMP180_H
#define BMP180_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_ERR_BMP180_BASE                  0x30000
#define ESP_ERR_TOO_SLOW_TICK_RATE           (ESP_ERR_BMP180_BASE + 1)
#define ESP_ERR_BMP180_NOT_DETECTED          (ESP_ERR_BMP180_BASE + 2)
#define ESP_ERR_BMP180_CALIBRATION_FAILURE   (ESP_ERR_BMP180_BASE + 3)

esp_err_t bmp180_init(void);
esp_err_t bmp180_read_temperature(float* temperature);
esp_err_t bmp180_read_pressure(uint32_t* pressure);
esp_err_t bmp180_read_altitude(uint32_t reference_pressure, float* altitude);

#ifdef __cplusplus
}
#endif

#endif  // BMP180_H
