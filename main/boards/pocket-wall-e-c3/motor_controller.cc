
#include "motor_controller.h"

#include <algorithm>
#include <cstdlib>
#include <string>

#include <esp_log.h>

#include "config.h"
#include "mcp_server.h"

#define TAG "PocketServo"

namespace {
constexpr ledc_mode_t kSpeedMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kTimer = LEDC_TIMER_2;
constexpr ledc_timer_bit_t kDutyResolution = LEDC_TIMER_14_BIT;
constexpr uint32_t kMaxDuty = (1U << 14) - 1;

constexpr ledc_channel_t kLeftChannel = LEDC_CHANNEL_1;
constexpr ledc_channel_t kRightChannel = LEDC_CHANNEL_2;

int ClampSpeed(int speed) {
    return std::clamp(speed, -100, 100);
}

uint32_t ClampDuration(uint32_t duration_ms) {
    return std::clamp(duration_ms, uint32_t{100},
                      uint32_t{SERVO_MAX_RUN_TIME_MS});
}
}  // namespace

MotorController::MotorController(gpio_num_t left_pin, gpio_num_t right_pin)
    : left_pin_(left_pin), right_pin_(right_pin) {
    ConfigurePwm();

    // Beri sinyal netral sebelum menerima perintah gerak.
    ApplySpeeds(0, 0);

    command_queue_ = xQueueCreate(1, sizeof(Command));
    if (command_queue_ == nullptr) {
        ESP_LOGE(TAG, "Gagal membuat command queue");
        abort();
    }

    if (xTaskCreate(TaskEntry, "pocket_servo", 4096, this, 4,
                    &task_handle_) != pdPASS) {
        ESP_LOGE(TAG, "Gagal membuat task servo");
        abort();
    }

    RegisterMcpTools();
    ESP_LOGI(TAG, "Kontrol dua servo 360 derajat siap");
}

MotorController::~MotorController() {
    ApplySpeeds(0, 0);

    if (task_handle_ != nullptr) {
        vTaskDelete(task_handle_);
    }

    if (command_queue_ != nullptr) {
        vQueueDelete(command_queue_);
    }
}

void MotorController::ConfigurePwm() {
    ledc_timer_config_t timer_config = {};
    timer_config.speed_mode = kSpeedMode;
    timer_config.duty_resolution = kDutyResolution;
    timer_config.timer_num = kTimer;
    timer_config.freq_hz = SERVO_PWM_FREQUENCY_HZ;
    timer_config.clk_cfg = LEDC_AUTO_CLK;

    ESP_ERROR_CHECK(ledc_timer_config(&timer_config));

    const gpio_num_t pins[] = {left_pin_, right_pin_};
    const ledc_channel_t channels[] = {kLeftChannel, kRightChannel};

    for (int i = 0; i < 2; ++i) {
        ledc_channel_config_t channel_config = {};
        channel_config.gpio_num = pins[i];
        channel_config.speed_mode = kSpeedMode;
        channel_config.channel = channels[i];
        channel_config.intr_type = LEDC_INTR_DISABLE;
        channel_config.timer_sel = kTimer;
        channel_config.duty = 0;
        channel_config.hpoint = 0;
        channel_config.flags.output_invert = 0;

        ESP_ERROR_CHECK(ledc_channel_config(&channel_config));
    }
}

bool MotorController::Drive(int left_speed, int right_speed,
                            uint32_t duration_ms) {
    Command command = {
        .left_speed = ClampSpeed(left_speed),
        .right_speed = ClampSpeed(right_speed),
        .duration_ms = ClampDuration(duration_ms),
    };

    return xQueueOverwrite(command_queue_, &command) == pdPASS;
}

void MotorController::Stop() {
    if (command_queue_ != nullptr) {
        Command command = {};
        xQueueOverwrite(command_queue_, &command);
    }
}

bool MotorController::IsMoving() const {
    return moving_.load();
}

int MotorController::LeftSpeed() const {
    return left_speed_.load();
}

int MotorController::RightSpeed() const {
    return right_speed_.load();
}

void MotorController::TaskEntry(void* arg) {
    static_cast<MotorController*>(arg)->TaskLoop();
}

void MotorController::TaskLoop() {
    bool command_active = false;
    TickType_t deadline = 0;
    Command command = {};

    while (true) {
        TickType_t wait_ticks = portMAX_DELAY;

        if (command_active) {
            const TickType_t now = xTaskGetTickCount();
            const int32_t remaining =
                static_cast<int32_t>(deadline - now);

            if (remaining <= 0) {
                ApplySpeeds(0, 0);
                command_active = false;
                continue;
            }

            wait_ticks = static_cast<TickType_t>(remaining);
        }

        if (xQueueReceive(command_queue_, &command, wait_ticks) == pdTRUE) {
            ApplySpeeds(command.left_speed, command.right_speed);

            command_active =
                (command.left_speed != 0 || command.right_speed != 0) &&
                command.duration_ms != 0;

            if (command_active) {
                deadline = xTaskGetTickCount() +
                           pdMS_TO_TICKS(command.duration_ms);
            }
        } else if (command_active) {
            ApplySpeeds(0, 0);
            command_active = false;
        }
    }
}

void MotorController::ApplySpeeds(int left_speed, int right_speed) {
    left_speed = ClampSpeed(left_speed);
    right_speed = ClampSpeed(right_speed);

    SetServo(left_pin_, kLeftChannel, left_speed, SERVO_LEFT_INVERT);
    SetServo(right_pin_, kRightChannel, right_speed, SERVO_RIGHT_INVERT);

    left_speed_.store(left_speed);
    right_speed_.store(right_speed);
    moving_.store(left_speed != 0 || right_speed != 0);

    ESP_LOGI(TAG, "Servo kiri=%d kanan=%d", left_speed, right_speed);
}

void MotorController::SetServo(gpio_num_t pin, ledc_channel_t channel,
                               int speed, bool invert) {
    if (invert) {
        speed = -speed;
    }

    // Servo kontinu:
    // 0    = netral/berhenti
    // +100 = putaran satu arah
    // -100 = putaran arah sebaliknya
    //
    // Rentang awal 1000-2000 us. Netral 1500 us.
    const int pulse_us =
        SERVO_NEUTRAL_PULSE_US + (speed * 5);

    const uint32_t bounded_pulse = static_cast<uint32_t>(
        std::clamp(pulse_us, SERVO_MIN_PULSE_US, SERVO_MAX_PULSE_US));

    SetPulse(pin, channel, bounded_pulse);
}

void MotorController::SetPulse(gpio_num_t pin, ledc_channel_t channel,
                               uint32_t pulse_us) {
    (void)pin;

    // Duty dihitung dari lebar pulsa terhadap periode PWM 20.000 us.
    const uint32_t duty =
        (pulse_us * SERVO_PWM_FREQUENCY_HZ * kMaxDuty) / 1000000U;

    ESP_ERROR_CHECK(ledc_set_duty(kSpeedMode, channel, duty));
    ESP_ERROR_CHECK(ledc_update_duty(kSpeedMode, channel));
}

void MotorController::RegisterMcpTools() {
    auto& mcp_server = McpServer::GetInstance();

    mcp_server.AddTool(
        "self.wall_e.drive",
        "Move Wall-E's two-wheel base using continuous-rotation servos. "
        "direction must be forward, backward, left, or right. "
        "Left and right turn the whole robot using opposite wheel directions. "
        "Speed is from 0 to 100. Movement duration is limited for safety.",
        PropertyList({
            Property("direction", kPropertyTypeString, "forward"),
            Property("speed", kPropertyTypeInteger, 100, 0, 100),
            Property("duration_ms", kPropertyTypeInteger, 800, 100,
                     SERVO_MAX_RUN_TIME_MS),
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            const std::string direction =
                properties["direction"].value<std::string>();
            const int speed = ClampSpeed(
                properties["speed"].value<int>());
            const uint32_t duration = ClampDuration(
                static_cast<uint32_t>(
                    properties["duration_ms"].value<int>()));

            int left = 0;
            int right = 0;

            if (direction == "forward") {
                left = speed;
                right = speed;
            } else if (direction == "backward") {
                left = -speed;
                right = -speed;
            } else if (direction == "left") {
                left = -speed;
                right = speed;
            } else if (direction == "right") {
                left = speed;
                right = -speed;
            } else {
                return "Invalid direction. Use forward, backward, left, or right.";
            }

            if (!Drive(left, right, duration)) {
                return "Servo command queue is unavailable.";
            }

            ESP_LOGI(TAG, "Drive: %s speed=%d duration=%lu",
                     direction.c_str(), speed,
                     static_cast<unsigned long>(duration));

            return std::string("{\"ok\":true,\"direction\":\"") +
                   direction + "\",\"speed\":" + std::to_string(speed) +
                   ",\"duration_ms\":" + std::to_string(duration) + "}";
        });

    mcp_server.AddTool(
        "self.wall_e.run_motor",
        "Run exactly one wheel of Wall-E while keeping the other wheel stopped. "
        "motor must be left or right. direction must be forward or backward. "
        "Speed is from 0 to 100.",
        PropertyList({
            Property("motor", kPropertyTypeString, "left"),
            Property("direction", kPropertyTypeString, "forward"),
            Property("speed", kPropertyTypeInteger, 100, 0, 100),
            Property("duration_ms", kPropertyTypeInteger, 1000, 100,
                     SERVO_MAX_RUN_TIME_MS),
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            const std::string motor =
                properties["motor"].value<std::string>();
            const std::string direction =
                properties["direction"].value<std::string>();
            const int speed = ClampSpeed(
                properties["speed"].value<int>());
            const uint32_t duration = ClampDuration(
                static_cast<uint32_t>(
                    properties["duration_ms"].value<int>()));

            if (motor != "left" && motor != "right") {
                return "Invalid motor. Use left or right.";
            }

            if (direction != "forward" && direction != "backward") {
                return "Invalid direction. Use forward or backward.";
            }

            const int signed_speed =
                (direction == "forward") ? speed : -speed;

            const int left = (motor == "left") ? signed_speed : 0;
            const int right = (motor == "right") ? signed_speed : 0;

            if (!Drive(left, right, duration)) {
                return "Servo command queue is unavailable.";
            }

            return std::string("{\"ok\":true,\"motor\":\"") + motor +
                   "\",\"direction\":\"" + direction +
                   "\",\"speed\":" + std::to_string(speed) +
                   ",\"other_motor\":\"stopped\",\"duration_ms\":" +
                   std::to_string(duration) + "}";
        });

    mcp_server.AddTool(
        "self.wall_e.stop",
        "Stop both of Wall-E's continuous-rotation servos.",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            Stop();
            return true;
        });

    mcp_server.AddTool(
        "self.wall_e.get_status",
        "Get Wall-E servo movement status.",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            return std::string("{\"moving\":") +
                   (IsMoving() ? "true" : "false") +
                   ",\"left_speed\":" + std::to_string(LeftSpeed()) +
                   ",\"right_speed\":" + std::to_string(RightSpeed()) + "}";
        });
}
