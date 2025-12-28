#pragma once

#include "driver/gpio.h"

/**
 * @brief ESP32-WROOM-32D Board Configuration
 * * Hardware Notes:
 * - Internal WiFi antenna (No GPIO 3/14 switching needed)
 * - ADC1_CH0 is on GPIO 36 (VP)
 * - Minimum hardware sampling rate: 20,000 Hz
 */

 static inline void board_specific_init(void)
// No-op for WROOM-32D


