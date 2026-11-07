

#ifndef POCKET_WALL_E_C3_OBSTACLE_AVOIDANCE_H
#define POCKET_WALL_E_C3_OBSTACLE_AVOIDANCE_H

#include <atomic>
#include <cstdint>
#include <string>

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "motor_controller.h"
#include "mcp_server.h"

#include "config.h"

class ObstacleAvoidance {
private:
    static constexpr const char* TAG = "WallEAuto";

    static constexpr gpio_num_t TRIG_PIN = GPIO_NUM_0;
    static constexpr gpio_num_t ECHO_PIN = GPIO_NUM_3;

    static constexpr uint32_t AUTO_DURATION_MS = 60000;
    static constexpr uint32_t OBSTACLE_DISTANCE_CM = 20;

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

    uint32_t state_start_ms_ = 0;
    uint32_t auto_start_ms_ = 0;

    bool turn_right_ = true;

    static uint32_t Millis() {
        return static_cast<uint32_t>(
            esp_timer_get_time() / 1000
        );
    }

    static void TaskEntry(void* arg) {
        auto* self = static_cast<ObstacleAvoidance*>(arg);
        self->TaskLoop();
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
    }

    int ReadDistance() {
        gpio_set_level(TRIG_PIN, 0);
        esp_rom_delay_us(3);

        gpio_set_level(TRIG_PIN, 1);
        esp_rom_delay_us(10);
        gpio_set_level(TRIG_PIN, 0);

        int64_t timeout = esp_timer_get_time() + 25000;

        while (gpio_get_level(ECHO_PIN) == 0) {
            if (esp_timer_get_time() > timeout) {
                return -1;
            }
        }

        int64_t pulse_start = esp_timer_get_time();

        while (gpio_get_level(ECHO_PIN) == 1) {
            if (esp_timer_get_time() > timeout) {
                return -1;
            }
        }

        int64_t pulse_end = esp_timer_get_time();

        int64_t duration = pulse_end - pulse_start;

        return static_cast<int>(duration / 58);
    }

    void StartAuto() {
        if (active_.exchange(true)) {
            ESP_LOGW(TAG, "Autonomous mode already active");
            return;
        }

        emergency_stop_ = false;

        motor_->SetAutonomousMode(true);

        auto_start_ms_ = Millis();
        state_start_ms_ = auto_start_ms_;

        state_ = STATE_FORWARD;

        ESP_LOGI(TAG, "Autonomous mode started");
    }

    void FinishAuto() {
        motor_->Stop(false);
        motor_->SetAutonomousMode(false);

        active_ = false;

        ESP_LOGI(TAG, "Autonomous mode finished");
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

            uint32_t now = Millis();

            if (now - auto_start_ms_ >= AUTO_DURATION_MS) {
                FinishAuto();
                continue;
            }

            switch (state_) {

                case STATE_FORWARD: {
                    int distance = ReadDistance();

                    if (distance < 0) {
                        // Sensor tidak memberikan echo:
                        // hentikan gerakan untuk menghindari risiko.
                        ESP_LOGW(TAG, "Invalid ultrasonic reading");
                        motor_->Stop(false);
                        vTaskDelay(pdMS_TO_TICKS(100));
                        continue;
                    }

                    if (distance <= OBSTACLE_DISTANCE_CM) {
                        ESP_LOGI(TAG,
                                 "Obstacle detected: %d cm",
                                 distance);

                        motor_->Stop(false);

                        state_ = STATE_REVERSE;
                        state_start_ms_ = now;

                        motor_->Drive(-100, -100, 5000, true);
                    } else {
                        motor_->Drive(100, 100, 5000, true);
                    }

                    break;
                }

                case STATE_REVERSE: {
                    if (now - state_start_ms_ >= REVERSE_TIME_MS) {
                        motor_->Stop(false);

                        state_ = STATE_TURN;
                        state_start_ms_ = now;

                        if (turn_right_) {
                            motor_->Drive(100, -100, 5000, true);
                        } else {
                            motor_->Drive(-100, 100, 5000, true);
                        }

                        turn_right_ = !turn_right_;
                    }

                    break;
                }

                case STATE_TURN: {
                    if (now - state_start_ms_ >= TURN_TIME_MS) {
                        motor_->Stop(false);

                        state_ = STATE_CHECK;
                        state_start_ms_ = now;
                    }

                    break;
                }

                case STATE_CHECK: {
                    int distance = ReadDistance();

                    if (distance < 0 ||
                        distance <= OBSTACLE_DISTANCE_CM) {

                        // Jalur masih terhalang.
                        // Coba arah putaran berikutnya.
                        state_ = STATE_REVERSE;
                        state_start_ms_ = now;

                        motor_->Drive(-100, -100, 5000, true);
                    } else {
                        state_ = STATE_FORWARD;
                        state_start_ms_ = now;

                        motor_->Drive(100, 100, 5000, true);
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
            "Start Wall-E autonomous obstacle avoidance mode for exactly "
            "60 seconds. When the user says Auto, invoke this tool. "
            "Wall-E will announce: Wall-E akan jalan-jalan dulu.",
            PropertyList(),

            [this](const PropertyList&) -> ReturnValue {

                if (active_) {
                    return std::string(
                        "Wall-E sedang berjalan otomatis."
                    );
                }

                StartAuto();

                return std::string(
                    "Wall-E akan jalan-jalan dulu."
                );
            }
        );
    }

public:
    explicit ObstacleAvoidance(MotorController* motor)
        : motor_(motor) {

        InitializeSensor();

        RegisterMcpTool();

        xTaskCreate(
            TaskEntry,
            "walle_auto",
            4096,
            this,
            4,
            &task_handle_
        );

        ESP_LOGI(TAG, "Obstacle avoidance initialized");
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

#endif
