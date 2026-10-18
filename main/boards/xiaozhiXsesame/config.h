

#ifndef XIAOZHI_XSESAME_CONFIG_H
#define XIAOZHI_XSESAME_CONFIG_H

#include <driver/gpio.h>

// =====================================================
// AUDIO
// Mengikuti konfigurasi audio Pocket Wall-E C3.
// Pastikan sambungan mikrofon dan amplifier sesuai.
// =====================================================

#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_2
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_1
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_3
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_5

// =====================================================
// I2C: SSD1306 OLED + PCA9685
// Kedua modul berbagi satu bus I2C.
// SDA = GPIO6, SCL = GPIO7
// =====================================================

#define XSESAME_I2C_SDA_PIN GPIO_NUM_6
#define XSESAME_I2C_SCL_PIN GPIO_NUM_7
#define XSESAME_I2C_PORT    I2C_NUM_0
#define XSESAME_I2C_FREQ_HZ 400000

// OLED SSD1306 128x64
#define DISPLAY_SDA_PIN XSESAME_I2C_SDA_PIN
#define DISPLAY_SCL_PIN XSESAME_I2C_SCL_PIN
#define DISPLAY_WIDTH   128
#define DISPLAY_HEIGHT  64
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false

#define XSESAME_OLED_I2C_ADDRESS 0x3C

// PCA9685: 8 servo MG90S
#define XSESAME_PCA9685_ADDRESS 0x40
#define XSESAME_SERVO_COUNT     8
#define XSESAME_SERVO_FREQUENCY 50

// Rentang pulsa awal; perlu dikalibrasi sesuai servo.
#define XSESAME_SERVO_MIN_PULSE_US 500
#define XSESAME_SERVO_MAX_PULSE_US 2500

// =====================================================
// TOMBOL
// GPIO0: tombol kontrol pengguna
// =====================================================

#define ACTION_BUTTON_GPIO GPIO_NUM_0

#define XSESAME_LISTENING_LED_GPIO GPIO_NUM_8
#define XSESAME_LISTENING_LED_ON  0
#define XSESAME_LISTENING_LED_OFF 1
// =====================================================
// WAKE WORD
// Hi Lexin diatur melalui config.json.
// =====================================================

#endif // XIAOZHI_XSESAME_CONFIG_H
