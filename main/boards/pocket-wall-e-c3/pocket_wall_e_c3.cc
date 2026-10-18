#include "application.h"
#include "audio/codecs/no_audio_codec.h"
#include "button.h"
#include "config.h"
#include "pocket_oled_display.h"
#include "wifi_board.h"

extern "C" {
#include "vl53l0x.h"
}

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#include <esp_log.h>
#include <led_strip.h>
#include <led_strip_rmt.h>
#include <cstdint>
#include <climits>
#include <vector>

#include "motor_controller.h"

#define TAG "PocketWallEC3"

namespace {

class PocketAudioCodec : public NoAudioCodecDuplex {
private:
    int32_t previous_input_ = 0;
    int32_t previous_output_ = 0;
    int32_t gain_q12_ = 4096;

public:
    PocketAudioCodec(int sample_rate, gpio_num_t bclk, gpio_num_t ws,
                     gpio_num_t dout, gpio_num_t din)
        : NoAudioCodecDuplex(sample_rate, sample_rate, bclk, ws, dout, din) {}

protected:
    int Read(int16_t* dest, int samples) override {
        std::vector<int32_t> raw(samples);
        size_t bytes_read = 0;

        if (i2s_channel_read(rx_handle_, raw.data(),
                             raw.size() * sizeof(int32_t),
                             &bytes_read, 200) != ESP_OK) {
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

        if (target_gain_q12 < 2048) target_gain_q12 = 2048;
        if (target_gain_q12 > 24576) target_gain_q12 = 24576;

        if (target_gain_q12 < gain_q12_) {
            gain_q12_ = (gain_q12_ + target_gain_q12 * 3) / 4;
        } else {
            gain_q12_ = (gain_q12_ * 7 + target_gain_q12) / 8;
        }

        for (int i = 0; i < count; ++i) {
            const int32_t amplified = static_cast<int32_t>(
                (static_cast<int64_t>(filtered[i]) * gain_q12_) >> 12);

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

}  // namespace

class PocketWallEC3Board : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;

    Button action_button_;
    MotorController* motor_controller_ = nullptr;
    vl53l0x_handle_t floor_sensor_ = nullptr;

    struct Vl53TaskContext {
        vl53l0x_handle_t sensor;
        MotorController* motors;
    };

    static void Vl53TestTask(void* arg) {
        auto* context = static_cast<Vl53TaskContext*>(arg);
        uint32_t log_counter = 0;

        while (true) {
            vl53l0x_data_t data = {};

            esp_err_t err =
                vl53l0x_single_measure(context->sensor, &data);

            // Nilai meja yang diuji: sekitar 48–57 mm.
            // Nilai di atas 150 mm berarti tepi meja.
            // Pembacaan gagal juga dianggap kondisi berbahaya.
            const bool edge_detected =
                err != ESP_OK ||
                !data.valid ||
                data.distance_mm > 150;

            context->motors->SetEdgeDetected(edge_detected);

            if (log_counter == 0) {
                if (err == ESP_OK && data.valid) {
                    ESP_LOGI(
                        "VL53_TEST",
                        "Jarak: %u mm | Status: %u | Tepi: %s",
                        (unsigned)data.distance_mm,
                        (unsigned)data.range_status,
                        edge_detected ? "YA" : "tidak");
                } else {
                    ESP_LOGW(
                        "VL53_TEST",
                        "Pembacaan gagal: err=%s valid=%d "
                        "jarak=%u status=%u",
                        esp_err_to_name(err),
                        (int)data.valid,
                        (unsigned)data.distance_mm,
                        (unsigned)data.range_status);
                }
            }

            log_counter = (log_counter + 1) % 10;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    void InitializeVl53Test() {
        ESP_LOGI("VL53_TEST", "Memulai sensor VL53L0X");

        esp_err_t err =
            vl53l0x_create(&floor_sensor_, display_i2c_bus_);

        if (err != ESP_OK) {
            ESP_LOGE("VL53_TEST", "Gagal membuat sensor: %s",
                     esp_err_to_name(err));
            return;
        }

        err = vl53l0x_init(floor_sensor_);

        if (err != ESP_OK) {
            ESP_LOGE("VL53_TEST", "Inisialisasi sensor gagal: %s",
                     esp_err_to_name(err));
            return;
        }

        vl53l0x_ref_spad_calibration_t spad_cal = {};

        err = vl53l0x_perform_ref_spad_management(
            floor_sensor_, &spad_cal);

        if (err != ESP_OK) {
            ESP_LOGE("VL53_TEST", "Kalibrasi SPAD gagal: %s",
                     esp_err_to_name(err));
            return;
        }

        vl53l0x_ref_calibration_t ref_cal = {};

        err = vl53l0x_perform_ref_calibration(
            floor_sensor_, &ref_cal);

        if (err != ESP_OK) {
            ESP_LOGE("VL53_TEST", "Kalibrasi referensi gagal: %s",
                     esp_err_to_name(err));
            return;
        }

        ESP_LOGI("VL53_TEST",
                 "Kalibrasi OK: SPAD=%u aperture=%d",
                 (unsigned)spad_cal.count,
                 (int)spad_cal.is_aperture);

        err = vl53l0x_set_profile(
            floor_sensor_, VL53L0X_PROFILE_DEFAULT);

        if (err != ESP_OK) {
            ESP_LOGE("VL53_TEST", "Gagal mengatur profil: %s",
                     esp_err_to_name(err));
            return;
        }

        // Konteks static tetap tersedia selama task berjalan.
        static Vl53TaskContext context;
        context.sensor = floor_sensor_;
        context.motors = motor_controller_;

        BaseType_t result = xTaskCreate(
            Vl53TestTask,
            "vl53_test",
            4096,
            &context,
            3,
            nullptr);

        if (result != pdPASS) {
            ESP_LOGE("VL53_TEST", "Gagal membuat task sensor");
        }
    }

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
            i2c_new_master_bus(&bus_config, &display_i2c_bus_));
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
                display_i2c_bus_, &io_config, &panel_io_));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;

#ifdef SH1106
        ESP_LOGI(TAG, "Installing SH1106 OLED driver");

        ESP_ERROR_CHECK(
            esp_lcd_new_panel_sh1106(
                panel_io_, &panel_config, &panel_));
#else
        esp_lcd_panel_ssd1306_config_t ssd1306_config = {};
        ssd1306_config.height = DISPLAY_HEIGHT;
        panel_config.vendor_config = &ssd1306_config;

        ESP_LOGI(TAG, "Installing SSD1306 OLED driver");

        ESP_ERROR_CHECK(
            esp_lcd_new_panel_ssd1306(
                panel_io_, &panel_config, &panel_));
#endif

        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, true));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new PocketOledDisplay(
            panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    }

    void InitializeButton() {
        action_button_.OnClick([this]() {
            auto& app = Application::GetInstance();

            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }

            app.ToggleChatState();
        });

        action_button_.OnLongPress(
            [this]() { EnterWifiConfigMode(); });
    }

public:
    PocketWallEC3Board() : action_button_(ACTION_BUTTON_GPIO) {
        InitializeDisplayI2c();
        InitializeDisplay();
        InitializeButton();

        // Motor dibuat lebih dahulu supaya task sensor memiliki
        // objek MotorController yang valid.
        motor_controller_ = new MotorController(
            MOTOR_LEFT_IN1_PIN,
            MOTOR_LEFT_IN2_PIN,
            MOTOR_RIGHT_IN1_PIN,
            MOTOR_RIGHT_IN2_PIN);

        InitializeVl53Test();

        ESP_LOGI(TAG, "Pocket Wall-E C3 initialized");
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
};

DECLARE_BOARD(PocketWallEC3Board);
