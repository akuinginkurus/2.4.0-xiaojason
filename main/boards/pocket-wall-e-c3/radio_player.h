
#pragma once

#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class RadioPlayer {
public:
    RadioPlayer();
    ~RadioPlayer();

    // station: nomor 1..N (urutan kStations); 0 = stasiun pertama.
    bool StartRadio(int station = 0);
    void RequestStop();

    bool IsPlaying() const {
        return active_.load();
    }

private:
    static void TaskEntry(void* arg);
    void TaskLoop();

    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> active_{false};
    std::atomic<int> requested_station_{0};  // indeks 0-based stasiun awal

    // 0 = menunggu audio pertama
    // 1 = PCM berhasil dikirim
    // -1 = gagal memulai radio
    std::atomic<int> startup_state_{0};

    TaskHandle_t task_handle_ = nullptr;
};
