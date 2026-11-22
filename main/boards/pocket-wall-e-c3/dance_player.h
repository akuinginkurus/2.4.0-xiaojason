#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include <esp_timer.h>
#include <led_strip.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "config.h"

class MotorController;

// Memutar joget.ogg (tertanam di flash) sambil menggerakkan robot sampai lagu selesai.
// Tombol/perintah stop menghentikan lagu dan motor seketika.
class DancePlayer {
public:
    DancePlayer() = default;
    ~DancePlayer();

    // motor boleh nullptr (lagu tetap diputar tanpa gerak).
    bool Start(MotorController* motor);
    void RequestStop();

    bool IsActive() const {
        return active_.load();
    }

private:
    static void TaskEntry(void* arg);
    static void TimerEntry(void* arg);
    void TaskLoop();
    void WaitForAssistantReply();
    void CloseChatSession();
    void OnTick();
    void StepMotors();
    void StepLights();
    void StopMotors();
    void StartLights();
    void StopLights();

    std::atomic<bool> active_{false};
    std::atomic<bool> stop_requested_{false};
    MotorController* motor_ = nullptr;
    esp_timer_handle_t timer_ = nullptr;

    // Lampu WS2812B: strip dibuat saat joget mulai dan dihapus saat selesai (tanpa RAM
    // tetap). Mutex menjaga agar callback timer tidak memakai strip yang sedang dihapus.
    std::mutex lights_mutex_;
    led_strip_handle_t lights_ = nullptr;
    uint8_t light_pixels_[DANCE_LED_COUNT][3] = {};
    int light_pos_ = 0;
    int light_dir_ = 1;
    uint8_t light_hue_ = 0;
    uint32_t light_rng_ = 0x4A4F4745;

    // Hanya disentuh timer setelah task mengaturnya sebelum timer dijalankan.
    int64_t dance_start_us_ = 0;
    int64_t next_change_ms_ = 0;
    size_t step_index_ = 0;
};
