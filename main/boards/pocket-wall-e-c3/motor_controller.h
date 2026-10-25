
#ifndef _POCKET_WALL_E_C3_MOTOR_CONTROLLER_H_
#define _POCKET_WALL_E_C3_MOTOR_CONTROLLER_H_

#include <atomic>
#include <cstdint>

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

class MotorController {
public:
    MotorController(gpio_num_t left_pin, gpio_num_t right_pin);
    ~MotorController();

    bool Drive(int left_speed, int right_speed, uint32_t duration_ms);
    void Stop();

    bool IsMoving() const;
    int LeftSpeed() const;
    int RightSpeed() const;

private:
    struct Command {
        int left_speed;
        int right_speed;
        uint32_t duration_ms;
    };

    gpio_num_t left_pin_;
    gpio_num_t right_pin_;

    QueueHandle_t command_queue_ = nullptr;
    TaskHandle_t task_handle_ = nullptr;

    std::atomic<bool> moving_{false};
    std::atomic<int> left_speed_{0};
    std::atomic<int> right_speed_{0};

    static void TaskEntry(void* arg);
    void TaskLoop();
    void ConfigurePwm();
    void ApplySpeeds(int left_speed, int right_speed);
    void SetServo(gpio_num_t pin, ledc_channel_t channel, int speed, bool invert);
    void SetPulse(gpio_num_t pin, ledc_channel_t channel, uint32_t pulse_us);
    void RegisterMcpTools();
};

#endif
