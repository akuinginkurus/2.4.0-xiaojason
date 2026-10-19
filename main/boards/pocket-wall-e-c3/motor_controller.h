#ifndef _MOCHAN_V2_MOTOR_CONTROLLER_H_
#define _MOCHAN_V2_MOTOR_CONTROLLER_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

class MotorController {
public:
    MotorController(gpio_num_t left_in1, gpio_num_t left_in2,
                    gpio_num_t right_in1, gpio_num_t right_in2);
    ~MotorController();

    bool Drive(int left_speed, int right_speed, uint32_t duration_ms);
    void Stop();

    // Pengaman tepi meja
    void SetEdgeDetected(bool detected);
    bool IsEdgeDetected() const;

    bool IsMoving() const;
    int LeftSpeed() const;
    int RightSpeed() const;

private:
    struct Command {
        int left_speed;
        int right_speed;
        uint32_t duration_ms;
    };

    gpio_num_t pins_[4];

    const ledc_channel_t channels_[4] = {
        LEDC_CHANNEL_1,
        LEDC_CHANNEL_2,
        LEDC_CHANNEL_3,
        LEDC_CHANNEL_4,
    };

    QueueHandle_t command_queue_ = nullptr;
    TaskHandle_t task_handle_ = nullptr;

    std::atomic<bool> moving_{false};
    std::atomic<bool> edge_detected_{true};
    std::atomic<int> left_speed_{0};
    std::atomic<int> right_speed_{0};

    int last_physical_left_speed_ = 0;
    int last_physical_right_speed_ = 0;

    static void TaskEntry(void* arg);
    void TaskLoop();
    void ConfigurePwm();
    void ApplySpeeds(int left_speed, int right_speed);
    void SetMotor(int speed, ledc_channel_t forward_channel,
                  ledc_channel_t reverse_channel);
    void SetDuty(ledc_channel_t channel, uint32_t duty);
    void RegisterMcpTools();
};

#endif  // _MOCHAN_V2_MOTOR_CONTROLLER_H_
