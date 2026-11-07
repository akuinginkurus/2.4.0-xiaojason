
#include "application.h"
#include "audio/codecs/no_audio_codec.h"
#include "button.h"
#include "config.h"
#include "pocket_oled_display.h"
#include "wifi_board.h"

#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_err.h>

#include "led/led.h"
#include "youtube_audio_poller.h"

#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#include <esp_log.h>
#include <cstdint>
#include <climits>
#include <vector>

#include "motor_controller.h"
#include "joystick_espnow.h"

#define TAG "PocketWallEC3"

namespace {

// ============================================================
// Battery ADC reader
// Battery: 1S Li-ion / LiPo
// Divider: 10k from Battery+ to GPIO1, 10k from GPIO1 to GND
// GPIO1 = ADC1_CHANNEL_1 on ESP32-C3
// ============================================================

class BatteryAdcReader {
private:
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    adc_cali_handle_t calibration_handle_ = nullptr;

    bool ready_ = false;
    bool calibrated_ = false;

public:
    BatteryAdcReader() {
        adc_oneshot_unit_init_cfg_t unit_config = {};
        unit_config.unit_id = ADC_UNIT_1;
        unit_config.ulp_mode = ADC_ULP_MODE_DISABLE;

        if (adc_oneshot_new_unit(&unit_config, &adc_handle_) != ESP_OK) {
            ESP_LOGE(TAG, "Battery ADC initialization failed");
            return;
        }

        adc_oneshot_chan_cfg_t channel_config = {};
        channel_config.atten = ADC_ATTEN_DB_12;
        channel_config.bitwidth = ADC_BITWIDTH_DEFAULT;

        if (adc_oneshot_config_channel(
                adc_handle_,
                ADC_CHANNEL_1,
                &channel_config) != ESP_OK) {
            ESP_LOGE(TAG, "Battery ADC channel configuration failed");
            return;
        }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_curve_fitting_config_t calibration_config = {};
        calibration_config.unit_id = ADC_UNIT_1;
        calibration_config.chan = ADC_CHANNEL_1;
        calibration_config.atten = ADC_ATTEN_DB_12;
        calibration_config.bitwidth = ADC_BITWIDTH_DEFAULT;

        if (adc_cali_create_scheme_curve_fitting(
                &calibration_config,
                &calibration_handle_) == ESP_OK) {
            calibrated_ = true;
        }
#endif

        ready_ = true;

        ESP_LOGI(TAG,
                 "Battery ADC ready, calibration: %s",
                 calibrated_ ? "enabled" : "fallback");
    }

    bool ReadBatteryPercent(int& percentage) {
        if (!ready_) {
            return false;
        }

        // Read only five samples when requested.
        constexpr int sample_count = 5;
        int raw_sum = 0;

        for (int i = 0; i < sample_count; ++i) {
            int raw = 0;

            if (adc_oneshot_read(
                    adc_handle_,
                    ADC_CHANNEL_1,
                    &raw) != ESP_OK) {
                ESP_LOGW(TAG, "Battery ADC read failed");
                return false;
            }

            raw_sum += raw;
        }

        const int average_raw = raw_sum / sample_count;

        int adc_millivolts = 0;

        if (calibrated_ &&
            adc_cali_raw_to_voltage(
                calibration_handle_,
                average_raw,
                &adc_millivolts) == ESP_OK) {
            // Calibrated ADC voltage.
        } else {
            // Approximate fallback if calibration data is unavailable.
            adc_millivolts = (average_raw * 2500) / 4095;
        }

        // Divider 10k : 10k means battery voltage is twice ADC voltage.
        const float battery_voltage =
            static_cast<float>(adc_millivolts * 2) / 1000.0f;

        // Approximate 1S Li-ion voltage-to-charge curve.
        // This is an estimate, not a direct mAh measurement.
        struct VoltagePoint {
            float voltage;
            int percent;
        };

        static constexpr VoltagePoint curve[] = {
            {3.30f, 0},
            {3.50f, 5},
            {3.60f, 10},
            {3.65f, 15},
            {3.70f, 20},
            {3.75f, 35},
            {3.80f, 50},
            {3.85f, 65},
            {3.90f, 75},
            {4.00f, 85},
            {4.10f, 95},
            {4.20f, 100}
        };

        constexpr size_t curve_size =
            sizeof(curve) / sizeof(curve[0]);

        if (battery_voltage <= curve[0].voltage) {
            percentage = 0;
        } else if (battery_voltage >= curve[curve_size - 1].voltage) {
            percentage = 100;
        } else {
            for (size_t i = 1; i < curve_size; ++i) {
                if (battery_voltage <= curve[i].voltage) {
                    const float voltage_span =
                        curve[i].voltage - curve[i - 1].voltage;

                    const float ratio =
                        (battery_voltage - curve[i - 1].voltage) /
                        voltage_span;

                    percentage = static_cast<int>(
                        curve[i - 1].percent +
                        ratio *
                        (curve[i].percent - curve[i - 1].percent) +
                        0.5f);

                    break;
                }
            }
        }

        ESP_LOGI(TAG,
                 "Battery voltage: %.3f V, level: %d%%",
                 battery_voltage,
                 percentage);

        return true;
    }
};


// ============================================================
// Audio codec
// ============================================================

class PocketAudioCodec : public NoAudioCodecDuplex {
private:
    int32_t previous_input_ = 0;
    int32_t previous_output_ = 0;
    int32_t gain_q12_ = 4096;

public:
    PocketAudioCodec(int sample_rate,
                     gpio_num_t bclk,
                     gpio_num_t ws,
                     gpio_num_t dout,
                     gpio_num_t din)
        : NoAudioCodecDuplex(
              sample_rate,
              sample_rate,
              bclk,
              ws,
              dout,
              din) {}

protected:
    int Read(int16_t* dest, int samples) override {
        std::vector<int32_t> raw(samples);
        size_t bytes_read = 0;

        if (i2s_channel_read(
                rx_handle_,
                raw.data(),
                raw.size() * sizeof(int32_t),
                &bytes_read,
                200) != ESP_OK) {
            return 0;
        }

        const int count =
            static_cast<int>(bytes_read / sizeof(int32_t));

        std::vector<int32_t> filtered(count);
        int32_t peak = 1;

        for (int i = 0; i < count; ++i) {
            const int32_t input = raw[i] >> 14;

            const int32_t high_pass =
                input - previous_input_ +
                static_cast<int32_t>(
                    (static_cast<int64_t>(previous_output_) * 32604) >> 15);

            previous_input_ = input;
            previous_output_ = high_pass;
            filtered[i] = high_pass;

            const int32_t magnitude =
                high_pass < 0 ? -high_pass : high_pass;

            if (magnitude > peak) {
                peak = magnitude;
            }
        }

        int32_t target_gain_q12 =
            static_cast<int32_t>((14000LL << 12) / peak);

        if (target_gain_q12 < 2048) {
            target_gain_q12 = 2048;
        }

        if (target_gain_q12 > 24576) {
            target_gain_q12 = 24576;
        }

        if (target_gain_q12 < gain_q12_) {
            gain_q12_ =
                (gain_q12_ + target_gain_q12 * 3) / 4;
        } else {
            gain_q12_ =
                (gain_q12_ * 7 + target_gain_q12) / 8;
        }

        for (int i = 0; i < count; ++i) {
            const int32_t amplified =
                static_cast<int32_t>(
                    (static_cast<int64_t>(filtered[i]) *
                     gain_q12_) >> 12);

            if (amplified > INT16_MAX) {
                dest[i] = INT16_MAX;
            } else if (amplified < INT16_MIN) {
                dest[i] = INT16_MIN;
            } else {
                dest[i] = static_cast<int16_t>(amplified);
            }
        }

        return count;
    }
};


// ============================================================
// Listening LED
// ============================================================

class ListeningLed : public Led {
private:
    gpio_num_t gpio_;
    bool invert_;

    void Set(bool on) {
        gpio_set_level(gpio_, (on != invert_) ? 1 : 0);
    }

public:
    ListeningLed(gpio_num_t gpio, bool invert = false)
        : gpio_(gpio), invert_(invert) {
        gpio_reset_pin(gpio_);
        gpio_set_direction(gpio_, GPIO_MODE_OUTPUT);
        Set(false);
    }

    void OnStateChanged() override {
        auto state =
            Application::GetInstance().GetDeviceState();

        Set(state == kDeviceStateListening);
    }
};

}  // namespace


// ============================================================
// Pocket Wall-E C3 board
// ============================================================

class PocketWallEC3Board : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    Display* display_ = nullptr;

    Button action_button_;

    MotorController* motor_controller_ = nullptr;
    JoystickEspNow* joystick_espnow_ = nullptr;


    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {};

        bus_config.i2c_port = I2C_NUM_0;
        bus_config.sda_io_num = DISPLAY_SDA_PIN;
        bus_config.scl_io_num = DISPLAY_SCL_PIN;
        bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_config.glitch_ignore_cnt = 7;
        bus_config.intr_priority = 0;
        bus_config.trans_queue_depth = 0;
        bus_config.flags.enable_internal_pullup = 1;

        ESP_ERROR_CHECK(
            i2c_new_master_bus(
                &bus_config,
                &display_i2c_bus_));
    }


    void InitializeDisplay() {
        esp_lcd_panel_io_i2c_config_t io_config = {};

        io_config.dev_addr = 0x3C;
        io_config.scl_speed_hz = 400 * 1000;
        io_config.control_phase_bytes = 1;
        io_config.dc_bit_offset = 6;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;

        ESP_ERROR_CHECK(
            esp_lcd_new_panel_io_i2c(
                display_i2c_bus_,
                &io_config,
                &panel_io_));

        esp_lcd_panel_dev_config_t panel_config = {};

        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;

#ifdef SH1106
        ESP_LOGI(TAG, "Installing SH1106 OLED driver");

        ESP_ERROR_CHECK(
            esp_lcd_new_panel_sh1106(
                panel_io_,
                &panel_config,
                &panel_));
#else
        esp_lcd_panel_ssd1306_config_t ssd1306_config = {};

        ssd1306_config.height = DISPLAY_HEIGHT;
        panel_config.vendor_config = &ssd1306_config;

        ESP_LOGI(TAG, "Installing SSD1306 OLED driver");

        ESP_ERROR_CHECK(
            esp_lcd_new_panel_ssd1306(
                panel_io_,
                &panel_config,
                &panel_));
#endif

        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, true));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new PocketOledDisplay(
            panel_io_,
            panel_,
            DISPLAY_WIDTH,
            DISPLAY_HEIGHT,
            DISPLAY_MIRROR_X,
            DISPLAY_MIRROR_Y);
    }


    void InitializeButton() {
        action_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            auto& player = YouTubeAudioPoller::GetInstance();

            // Stop current YouTube playback on short press.
            if (player.IsPlaying()) {
                ESP_LOGI(
                    TAG,
                    "Button pressed: stopping YouTube playback");

                player.Abort();
                return;
            }

            // Preserve normal button behavior.
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }

            app.ToggleChatState();
        });

        action_button_.OnLongPress([this]() {
            EnterWifiConfigMode();
        });
    }


public:
    PocketWallEC3Board()
        : action_button_(ACTION_BUTTON_GPIO) {

        InitializeDisplayI2c();
        InitializeDisplay();
        InitializeButton();

        motor_controller_ = new MotorController(
            MOTOR_LEFT_IN1_PIN,
            MOTOR_LEFT_IN2_PIN,
            MOTOR_RIGHT_IN1_PIN,
            MOTOR_RIGHT_IN2_PIN);

        joystick_espnow_ =
            new JoystickEspNow(motor_controller_);

        ESP_LOGI(TAG, "Pocket Wall-E C3 initialized");
    }


    Led* GetLed() override {
        static ListeningLed led(
            BUILTIN_LED_GPIO,
            false);

        return &led;
    }


    AudioCodec* GetAudioCodec() override {
        static_assert(
            AUDIO_INPUT_SAMPLE_RATE == AUDIO_OUTPUT_SAMPLE_RATE,
            "Full-duplex shared I2S requires matching RX/TX rates");

        static PocketAudioCodec audio_codec(
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN);

        return &audio_codec;
    }


    Display* GetDisplay() override {
        return display_;
    }


    bool GetBatteryLevel(
        int& level,
        bool& charging,
        bool& discharging) override {

        // Lazy initialization: ADC is initialized only when first requested.
        static BatteryAdcReader battery_reader;

        charging = false;
        discharging = false;

        return battery_reader.ReadBatteryPercent(level);
    }
};


DECLARE_BOARD(PocketWallEC3Board);
