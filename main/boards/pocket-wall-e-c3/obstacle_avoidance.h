
#ifndef POCKET_WALL_E_C3_OBSTACLE_AVOIDANCE_H
#define POCKET_WALL_E_C3_OBSTACLE_AVOIDANCE_H

#include <atomic>
#include <cstdint>
#include <cstdlib>

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_rom_sys.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "motor_controller.h"
#include "mcp_server.h"

#define WALL_E_AUTO_TAG "WallEAuto"

class ObstacleAvoidance {
private:
    static constexpr gpio_num_t TRIG_PIN = ULTRASONIC_TRIG_PIN;
    static constexpr gpio_num_t ECHO_PIN = ULTRASONIC_ECHO_PIN;

    static constexpr uint32_t AUTO_DURATION_MS = 60000;
    static constexpr uint32_t OBSTACLE_DISTANCE_CM =
    ULTRASONIC_OBSTACLE_DISTANCE_CM;

    static constexpr uint32_t REVERSE_TIME_MS = 450;
    static constexpr uint32_t TURN_TIME_MS = 550;

    MotorController* motor_;

    std::atomic<bool> active_{false};
    std::atomic<bool> emergency_stop_{false};

    TaskHandle_t task_handle_ = nullptr;

    enum State {
        STATE_FORWARD,
        STATE_REVERSE,
        STATE_TURN,
        STATE_CHECK
    };

    State state_ = STATE_FORWARD;

    uint32_t auto_start_ms_ = 0;
    uint32_t state_start_ms_ = 0;

    bool turn_right_ = true;

    static uint32_t Millis() {
        return static_cast<uint32_t>(
            esp_timer_get_time() / 1000);
    }

    static void TaskEntry(void* arg) {
        auto* self =
            static_cast<ObstacleAvoidance*>(arg);

        self->TaskLoop();

        self->task_handle_ = nullptr;
        vTaskDelete(nullptr);
    }

    void InitializeSensor() {
        gpio_config_t trig = {};

        trig.pin_bit_mask = 1ULL << TRIG_PIN;
        trig.mode = GPIO_MODE_OUTPUT;
        trig.pull_up_en = GPIO_PULLUP_DISABLE;
        trig.pull_down_en = GPIO_PULLDOWN_DISABLE;
        trig.intr_type = GPIO_INTR_DISABLE;

        ESP_ERROR_CHECK(gpio_config(&trig));

        gpio_config_t echo = {};

        echo.pin_bit_mask = 1ULL << ECHO_PIN;
        echo.mode = GPIO_MODE_INPUT;
        echo.pull_up_en = GPIO_PULLUP_DISABLE;
        echo.pull_down_en = GPIO_PULLDOWN_DISABLE;
        echo.intr_type = GPIO_INTR_DISABLE;

        ESP_ERROR_CHECK(gpio_config(&echo));

        gpio_set_level(TRIG_PIN, 0);

        ESP_LOGI(
            WALL_E_AUTO_TAG,
            "HC-SR04 initialized: TRIG=%d ECHO=%d",
            static_cast<int>(TRIG_PIN),
            static_cast<int>(ECHO_PIN));
    }

    int ReadDistance() {
        gpio_set_level(TRIG_PIN, 0);
        esp_rom_delay_us(3);

        gpio_set_level(TRIG_PIN, 1);
        esp_rom_delay_us(10);
        gpio_set_level(TRIG_PIN, 0);

        const int64_t timeout =
            esp_timer_get_time() + 25000;

        while (gpio_get_level(ECHO_PIN) == 0) {
            if (esp_timer_get_time() > timeout) {
                return -1;
            }
        }

        const int64_t pulse_start =
            esp_timer_get_time();

        while (gpio_get_level(ECHO_PIN) == 1) {
            if (esp_timer_get_time() > timeout) {
                return -1;
            }
        }

        const int64_t pulse_end =
            esp_timer_get_time();

        const int64_t pulse_duration =
            pulse_end - pulse_start;

        return static_cast<int>(
            pulse_duration / 58);
    }

    bool StartAuto() {
        if (active_.exchange(true)) {
            return false;
        }

        emergency_stop_ = false;

        motor_->SetAutonomousMode(true);

        // Hapus perintah manual yang mungkin masih berjalan.
        motor_->Stop(false);

        auto_start_ms_ = Millis();
        state_start_ms_ = auto_start_ms_;

        state_ = STATE_FORWARD;
        turn_right_ = true;

        ESP_LOGI(
            WALL_E_AUTO_TAG,
            "Autonomous mode started for 60 seconds");

        return true;
    }

    void FinishAuto() {
        motor_->Stop(false);
        motor_->SetAutonomousMode(false);

        active_ = false;
        emergency_stop_ = false;

        ESP_LOGI(
            WALL_E_AUTO_TAG,
            "Autonomous mode finished; motor stopped");
    }

    void TaskLoop() {
        while (true) {
            if (!active_) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            if (emergency_stop_) {
                FinishAuto();
                continue;
            }

            const uint32_t now = Millis();

            if (now - auto_start_ms_ >= AUTO_DURATION_MS) {
                FinishAuto();
                continue;
            }

            switch (state_) {

                case STATE_FORWARD: {
                    const int distance = ReadDistance();

                    if (distance < 0) {
                        // Pembacaan tidak valid:
                        // berhenti, jangan bergerak tanpa sensor.
                        motor_->Stop(false);

                        ESP_LOGW(
                            WALL_E_AUTO_TAG,
                            "Ultrasonic timeout; motor stopped");

                        break;
                    }

                    if (distance <= OBSTACLE_DISTANCE_CM) {
                        ESP_LOGI(
                            WALL_E_AUTO_TAG,
                            "Obstacle detected: %d cm",
                            distance);

                        motor_->Stop(false);

                        state_ = STATE_REVERSE;
                        state_start_ms_ = now;

                        motor_->Drive(
                            -100, -100, 5000, true);

                    } else {
                        motor_->Drive(
                            100, 100, 5000, true);
                    }

                    break;
                }

                case STATE_REVERSE: {
                    if (now - state_start_ms_ >=
                        REVERSE_TIME_MS) {

                        motor_->Stop(false);

                        state_ = STATE_TURN;
                        state_start_ms_ = now;

                        if (turn_right_) {
                            motor_->Drive(
                                100, -100, 5000, true);
                        } else {
                            motor_->Drive(
                                -100, 100, 5000, true);
                        }

                        turn_right_ = !turn_right_;
                    }

                    break;
                }

                case STATE_TURN: {
                    if (now - state_start_ms_ >=
                        TURN_TIME_MS) {

                        motor_->Stop(false);

                        state_ = STATE_CHECK;
                        state_start_ms_ = now;
                    }

                    break;
                }

                case STATE_CHECK: {
                    const int distance = ReadDistance();

                    if (distance < 0 ||
                        distance <= OBSTACLE_DISTANCE_CM) {

                        ESP_LOGI(
                            WALL_E_AUTO_TAG,
                            "Path still blocked; trying other direction");

                        state_ = STATE_REVERSE;
                        state_start_ms_ = now;

                        motor_->Drive(
                            -100, -100, 5000, true);

                    } else {
                        ESP_LOGI(
                            WALL_E_AUTO_TAG,
                            "New path is clear: %d cm",
                            distance);

                        state_ = STATE_FORWARD;
                        state_start_ms_ = now;

                        motor_->Drive(
                            100, 100, 5000, true);
                    }

                    break;
                }
            }

            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    void RegisterMcpTool() {
        auto& mcp = McpServer::GetInstance();

        mcp.AddTool(
            "self.wall_e.auto",
            "Start Wall-E autonomous obstacle avoidance mode for "
            "60 seconds. When the user says Auto or asks Wall-E "
            "to walk around autonomously, invoke this tool. "
            "Wall-E should tell the user: "
            "Wall-E akan jalan-jalan dulu.",
            PropertyList(),

            [this](const PropertyList&) -> ReturnValue {
                if (!StartAuto()) {
                    return std::string(
                        "Wall-E sedang berjalan otomatis.");
                }

                return std::string(
                    "Wall-E akan jalan-jalan dulu.");
            });
    }

public:
    explicit ObstacleAvoidance(MotorController* motor)
        : motor_(motor) {

        InitializeSensor();

        if (xTaskCreate(
                TaskEntry,
                "walle_auto",
                4096,
                this,
                4,
                &task_handle_) != pdPASS) {

            ESP_LOGE(
                WALL_E_AUTO_TAG,
                "Failed to create autonomous task");

            abort();
        }

        RegisterMcpTool();

        ESP_LOGI(
            WALL_E_AUTO_TAG,
            "Obstacle avoidance ready");
    }

    ~ObstacleAvoidance() {
        active_ = false;
        emergency_stop_ = true;

        motor_->Stop();

        if (task_handle_ != nullptr) {
            vTaskDelete(task_handle_);
            task_handle_ = nullptr;
        }
    }

    void EmergencyStop() {
        if (active_) {
            emergency_stop_ = true;
            motor_->Stop();
        }
    }

    bool IsActive() const {
        return active_.load();
    }
};

#undef WALL_E_AUTO_TAG

#endif
