#ifndef POCKET_WALL_E_C3_CONFIG_H
#define POCKET_WALL_E_C3_CONFIG_H

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE 24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// Shared-clock full-duplex I2S: INMP441 + MAX98357A.
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_2
#define AUDIO_I2S_GPIO_WS GPIO_NUM_1
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_3
#define AUDIO_I2S_GPIO_DIN GPIO_NUM_10

#define DISPLAY_SDA_PIN GPIO_NUM_21
#define DISPLAY_SCL_PIN GPIO_NUM_20
#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 64
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y true

#define BUILTIN_LED_GPIO GPIO_NUM_8

#define ACTION_BUTTON_GPIO GPIO_NUM_0

 // Continuous-rotation servo drive
#define SERVO_LEFT_PIN GPIO_NUM_5
#define SERVO_RIGHT_PIN GPIO_NUM_6

// Ubah menjadi true jika arah putaran roda terbalik.
#define SERVO_LEFT_INVERT false
#define SERVO_RIGHT_INVERT false

// Servo standar memakai sinyal sekitar 50 Hz.
#define SERVO_PWM_FREQUENCY_HZ 50
#define SERVO_NEUTRAL_PULSE_US 1500
#define SERVO_MIN_PULSE_US 1000
#define SERVO_MAX_PULSE_US 2000

// Batas waktu gerak maksimum: 5 detik.
#define SERVO_MAX_RUN_TIME_MS 5000

#endif  // POCKET_WALL_E_C3_CONFIG_H
