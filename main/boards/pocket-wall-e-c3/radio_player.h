#pragma once

#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class RadioPlayer {
public:
    RadioPlayer();
    ~RadioPlayer();

    bool StartPrambors();
    void RequestStop();

    bool IsPlaying() const {
        return active_.load();
    }

private:
    static void TaskEntry(void* arg);
    void TaskLoop();

    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> active_{false};

    // 0 = menunggu koneksi/audio pertama
    // 1 = radio berhasil mulai mengirim PCM
    // -1 = gagal memulai radio
    std::atomic<int> startup_state_{0};

    TaskHandle_t task_handle_ = nullptr;
};
