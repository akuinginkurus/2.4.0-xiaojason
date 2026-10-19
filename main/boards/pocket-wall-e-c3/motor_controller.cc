#include "motor_controller.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

#include <esp_log.h>

#include "config.h"
#include "mcp_server.h"

#define TAG "MochanMotor"

namespace {
constexpr ledc_mode_t kSpeedMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kTimer = LEDC_TIMER_2;
constexpr ledc_timer_bit_t kDutyResolution = LEDC_TIMER_10_BIT;
constexpr uint32_t kMaxDuty = (1U << 10) - 1;
}  // namespace

MotorController::MotorController(
    gpio_num_t left_in1, gpio_num_t left_in2,
    gpio_num_t right_in1, gpio_num_t right_in2)
    : pins_{left_in1, left_in2, right_in1, right_in2} {
    ConfigurePwm();
    ApplySpeeds(0, 0);

    command_queue_ = xQueueCreate(1, sizeof(Command));
    if (command_queue_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create command queue");
        abort();
    }

    if (xTaskCreate(TaskEntry, "mochan_motor", 4096, this, 4,
                    &task_handle_) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create motor task");
        abort();
    }

    RegisterMcpTools();
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
    timer_config.freq_hz = MOTOR_PWM_FREQUENCY_HZ;
    timer_config.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer_config));

    for (size_t i = 0; i < 4; ++i) {
        ledc_channel_config_t channel_config = {};
        channel_config.gpio_num = pins_[i];
        channel_config.speed_mode = kSpeedMode;
        channel_config.channel = channels_[i];
        channel_config.intr_type = LEDC_INTR_DISABLE;
        channel_config.timer_sel = kTimer;
        channel_config.duty = 0;
        channel_config.hpoint = 0;
        channel_config.flags.output_invert = 0;

        ESP_ERROR_CHECK(ledc_channel_config(&channel_config));
    }
}

void MotorController::SetMovementCheck(
    std::function<bool(int, int)> check) {
    movement_check_ = std::move(check);
}

bool MotorController::Drive(int left_speed, int right_speed,
                            uint32_t duration_ms) {
    // Saat tepi terdeteksi, hanya gerakan mundur dengan kedua roda
    // yang diperbolehkan. Maju dan belok diblokir.

    if ((left_speed != 0 || right_speed != 0) &&
    movement_check_) {
    SetEdgeDetected(
        movement_check_(left_speed, right_speed));
    }
    
    if (edge_detected_.load() &&
        (left_speed > 0 || right_speed > 0)) {
        ESP_LOGW(TAG, "Gerakan diblokir: tepi meja terdeteksi");
        return false;
    }

    auto apply_start_threshold = [](int speed) {
        speed = std::clamp(speed, -100, 100);

        if (speed > 0 && speed < MOTOR_MIN_DUTY_PERCENT) {
            return MOTOR_MIN_DUTY_PERCENT;
        }

        if (speed < 0 && speed > -MOTOR_MIN_DUTY_PERCENT) {
            return -MOTOR_MIN_DUTY_PERCENT;
        }

        return speed;
    };

    Command command = {
        .left_speed = apply_start_threshold(left_speed),
        .right_speed = apply_start_threshold(right_speed),
        .duration_ms = std::clamp(
            duration_ms,
            uint32_t{100},
            uint32_t{MOTOR_MAX_RUN_TIME_MS}),
    };

    return xQueueOverwrite(command_queue_, &command) == pdPASS;
}

void MotorController::Stop() {
    Command command = {};
    xQueueOverwrite(command_queue_, &command);
}

void MotorController::SetEdgeDetected(bool detected) {
    bool previous = edge_detected_.exchange(detected);

    if (detected && !previous) {
        ESP_LOGW(TAG, "TEPI MEJA TERDETEKSI! Motor dihentikan.");
        Stop();
    }
}

bool MotorController::IsEdgeDetected() const {
    return edge_detected_.load();
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
    Command command;

    while (true) {
        TickType_t wait_ticks = portMAX_DELAY;

        if (command_active) {
            TickType_t now = xTaskGetTickCount();
            int32_t ticks_remaining =
                static_cast<int32_t>(deadline - now);

            if (ticks_remaining <= 0) {
                ApplySpeeds(0, 0);
                command_active = false;
                continue;
            }

            wait_ticks = static_cast<TickType_t>(ticks_remaining);
        }

        if (xQueueReceive(command_queue_, &command, wait_ticks) == pdTRUE) {
            ApplySpeeds(command.left_speed, command.right_speed);

            command_active =
                (command.left_speed != 0 ||
                 command.right_speed != 0) &&
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
    left_speed = std::clamp(left_speed, -100, 100);
    right_speed = std::clamp(right_speed, -100, 100);

    int physical_left_speed = left_speed;
    int physical_right_speed = right_speed;

    if (MOTOR_LEFT_INVERT) {
        physical_left_speed = -physical_left_speed;
    }

    if (MOTOR_RIGHT_INVERT) {
        physical_right_speed = -physical_right_speed;
    }

    const bool reversing =
        (last_physical_left_speed_ != 0 &&
         physical_left_speed != 0 &&
         ((last_physical_left_speed_ > 0) !=
          (physical_left_speed > 0))) ||
        (last_physical_right_speed_ != 0 &&
         physical_right_speed != 0 &&
         ((last_physical_right_speed_ > 0) !=
          (physical_right_speed > 0)));

    if (reversing) {
        SetMotor(0, channels_[0], channels_[1]);
        SetMotor(0, channels_[2], channels_[3]);

        last_physical_left_speed_ = 0;
        last_physical_right_speed_ = 0;

        vTaskDelay(pdMS_TO_TICKS(MOTOR_DIRECTION_DEAD_TIME_MS));
    }

    const bool starting_both =
        last_physical_left_speed_ == 0 &&
        last_physical_right_speed_ == 0 &&
        physical_left_speed != 0 &&
        physical_right_speed != 0;

    SetMotor(physical_left_speed, channels_[0], channels_[1]);

    if (starting_both) {
        vTaskDelay(pdMS_TO_TICKS(MOTOR_DUAL_START_STAGGER_MS));
    }

    SetMotor(physical_right_speed, channels_[2], channels_[3]);

    last_physical_left_speed_ = physical_left_speed;
    last_physical_right_speed_ = physical_right_speed;

    left_speed_.store(left_speed);
    right_speed_.store(right_speed);
    moving_.store(left_speed != 0 || right_speed != 0);
}

void MotorController::SetMotor(
    int speed,
    ledc_channel_t forward_channel,
    ledc_channel_t reverse_channel) {
    uint32_t duty =
        static_cast<uint32_t>(std::abs(speed)) *
        MOTOR_MAX_DUTY_PERCENT * kMaxDuty / 10000U;

    if (speed > 0) {
        SetDuty(reverse_channel, 0);
        SetDuty(forward_channel, duty);
    } else if (speed < 0) {
        SetDuty(forward_channel, 0);
        SetDuty(reverse_channel, duty);
    } else {
        SetDuty(forward_channel, 0);
        SetDuty(reverse_channel, 0);
    }
}

void MotorController::SetDuty(ledc_channel_t channel, uint32_t duty) {
    ESP_ERROR_CHECK(ledc_set_duty(kSpeedMode, channel, duty));
    ESP_ERROR_CHECK(ledc_update_duty(kSpeedMode, channel));
}

void MotorController::RegisterMcpTools() {
    auto& mcp_server = McpServer::GetInstance();

    mcp_server.AddTool(
        "self.wall_e.drive",
        "Move Wall-E's two-wheel base. direction must be forward, backward, "
        "left, or right. Left/right turn the whole robot. Do not use this "
        "tool for only one motor; use self.wall_e.run_motor instead. "
        "Every non-zero motor command runs at 100% power. duration_ms is "
        "limited for safety.",
        PropertyList({
            Property("direction", kPropertyTypeString, "forward"),
            Property("speed", kPropertyTypeInteger, 100, 0, 100),
            Property("duration_ms", kPropertyTypeInteger, 800, 100,
                     MOTOR_MAX_RUN_TIME_MS),
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            const std::string direction =
                properties["direction"].value<std::string>();
            const int speed = properties["speed"].value<int>();
            const int duration_ms =
                properties["duration_ms"].value<int>();

            int left_speed = 0;
            int right_speed = 0;

            if (direction == "forward") {
                left_speed = speed;
                right_speed = speed;
            } else if (direction == "backward") {
                left_speed = -speed;
                right_speed = -speed;
            } else if (direction == "left") {
                left_speed = -speed;
                right_speed = speed;
            } else if (direction == "right") {
                left_speed = speed;
                right_speed = -speed;
            } else {
                return "Invalid direction. Use forward, backward, left, or right.";
            }

            if (IsEdgeDetected() &&
                (left_speed > 0 || right_speed > 0)) {
                return "Tepi meja terdeteksi. Maju/belok diblokir; mundur dahulu.";
            }

            if (!Drive(left_speed, right_speed,
                       static_cast<uint32_t>(duration_ms))) {
                return "Motor command queue is unavailable.";
            }

            const int applied_speed =
                speed == 0 ? 0 :
                std::max(speed, static_cast<int>(MOTOR_MIN_DUTY_PERCENT));

            ESP_LOGI(TAG,
                     "Drive robot: direction=%s requested=%d%% "
                     "applied=%d%% duration=%dms",
                     direction.c_str(), speed, applied_speed, duration_ms);

            return std::string("{\"ok\":true,\"direction\":\"") +
                   direction + "\",\"requested_speed\":" +
                   std::to_string(speed) + ",\"applied_speed\":" +
                   std::to_string(applied_speed) + ",\"duration_ms\":" +
                   std::to_string(duration_ms) + "}";
        });

    mcp_server.AddTool(
        "self.wall_e.run_motor",
        "Run exactly one of Wall-E's motors while keeping the other stopped. "
        "motor must be left or right. direction must be forward or backward. "
        "Every non-zero motor command runs at 100% power.",
        PropertyList({
            Property("motor", kPropertyTypeString, "left"),
            Property("direction", kPropertyTypeString, "forward"),
            Property("speed", kPropertyTypeInteger, 100, 0, 100),
            Property("duration_ms", kPropertyTypeInteger, 1000, 100,
                     MOTOR_MAX_RUN_TIME_MS),
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            const std::string motor =
                properties["motor"].value<std::string>();
            const std::string direction =
                properties["direction"].value<std::string>();
            const int speed = properties["speed"].value<int>();
            const int duration_ms =
                properties["duration_ms"].value<int>();

            if (motor != "left" && motor != "right") {
                return "Invalid motor. Use left or right.";
            }

            if (direction != "forward" && direction != "backward") {
                return "Invalid direction. Use forward or backward.";
            }

            const int signed_speed =
                direction == "forward" ? speed : -speed;
            const int left_speed =
                motor == "left" ? signed_speed : 0;
            const int right_speed =
                motor == "right" ? signed_speed : 0;

            if (IsEdgeDetected() &&
                (left_speed > 0 || right_speed > 0)) {
                return "Tepi meja terdeteksi. Gerakan maju diblokir; mundur dahulu.";
            }

            if (!Drive(left_speed, right_speed,
                       static_cast<uint32_t>(duration_ms))) {
                return "Motor command queue is unavailable.";
            }

            const int applied_speed =
                speed == 0 ? 0 :
                std::max(speed, static_cast<int>(MOTOR_MIN_DUTY_PERCENT));

            ESP_LOGI(TAG,
                     "Run one motor: motor=%s direction=%s "
                     "requested=%d%% applied=%d%% duration=%dms",
                     motor.c_str(), direction.c_str(), speed,
                     applied_speed, duration_ms);

            return std::string("{\"ok\":true,\"motor\":\"") +
                   motor + "\",\"direction\":\"" + direction +
                   "\",\"requested_speed\":" + std::to_string(speed) +
                   ",\"applied_speed\":" + std::to_string(applied_speed) +
                   ",\"other_motor\":\"stopped\",\"duration_ms\":" +
                   std::to_string(duration_ms) + "}";
        });

    mcp_server.AddTool(
        "self.wall_e.stop",
        "Stop Wall-E's motors immediately.",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            Stop();
            return true;
        });

    mcp_server.AddTool(
        "self.wall_e.get_status",
        "Get Wall-E motor status.",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            return std::string("{\"moving\":") +
                   (IsMoving() ? "true" : "false") +
                   ",\"left_speed\":" + std::to_string(LeftSpeed()) +
                   ",\"right_speed\":" + std::to_string(RightSpeed()) +
                   ",\"edge_detected\":" +
                   (IsEdgeDetected() ? "true" : "false") + "}";
        });
}
