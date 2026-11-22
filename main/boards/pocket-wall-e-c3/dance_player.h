#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

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
    void StepMotors();
    void StopMotors();

    std::atomic<bool> active_{false};
    std::atomic<bool> stop_requested_{false};
    MotorController* motor_ = nullptr;
    esp_timer_handle_t timer_ = nullptr;

    // Hanya disentuh timer setelah task mengaturnya sebelum timer dijalankan.
    int64_t dance_start_us_ = 0;
    int64_t next_change_ms_ = 0;
    size_t step_index_ = 0;
};
