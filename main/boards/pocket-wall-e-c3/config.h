#ifndef POCKET_WALL_E_C3_CONFIG_H
#define POCKET_WALL_E_C3_CONFIG_H

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE 24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// Shared-clock full-duplex I2S: INMP441 + MAX98357A.
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_2
#define AUDIO_I2S_GPIO_WS GPIO_NUM_1
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_3
#define AUDIO_I2S_GPIO_DIN GPIO_NUM_0

#define DISPLAY_SDA_PIN GPIO_NUM_21
#define DISPLAY_SCL_PIN GPIO_NUM_20
#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 64
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y true

#define BUILTIN_LED_GPIO GPIO_NUM_8

#define ACTION_BUTTON_GPIO GPIO_NUM_9

// MX1508 dual H-bridge.
#define MOTOR_LEFT_IN1_PIN GPIO_NUM_5
#define MOTOR_LEFT_IN2_PIN GPIO_NUM_6
#define MOTOR_RIGHT_IN1_PIN GPIO_NUM_7
#define MOTOR_RIGHT_IN2_PIN GPIO_NUM_10
#define MOTOR_LEFT_INVERT false
#define MOTOR_RIGHT_INVERT false
#define MOTOR_PWM_FREQUENCY_HZ 10000
#define MOTOR_MIN_DUTY_PERCENT 100
#define MOTOR_MAX_DUTY_PERCENT 100
#define MOTOR_MAX_RUN_TIME_MS 5000
#define MOTOR_DIRECTION_DEAD_TIME_MS 120
#define MOTOR_DUAL_START_STAGGER_MS 40

// Modul WS2812B (8 LED) untuk efek joget. Data di GPIO4, VCC 5V, GND disatukan dengan
// board. DANCE_LED_BRIGHTNESS (0-255) membatasi arus supaya catu daya tidak drop;
// 64 ~ 25% cukup terang dan menjaga arus total jauh di bawah 150 mA.
#define DANCE_LED_GPIO GPIO_NUM_4
#define DANCE_LED_COUNT 8
#define DANCE_LED_BRIGHTNESS 64

// Layar pembuka saat robot pertama dinyalakan (menggantikan tampilan standar xiaozhi).
// Ubah teks sesuai keinginan; isi BOOT_SPLASH_SUBTITLE dengan "" kalau tidak perlu baris kedua.
// Layar pembuka tampil minimal BOOT_SPLASH_MIN_MS, lalu hilang saat robot siap (standby).
// BOOT_SPLASH_MAX_MS = batas paling lama kalau robot tak kunjung siap.
#define BOOT_SPLASH_TITLE "Pocket WALL-E"
#define BOOT_SPLASH_SUBTITLE "Radio by Arie"
#define BOOT_SPLASH_MIN_MS 2500
#define BOOT_SPLASH_MAX_MS 25000

#endif  // POCKET_WALL_E_C3_CONFIG_H
