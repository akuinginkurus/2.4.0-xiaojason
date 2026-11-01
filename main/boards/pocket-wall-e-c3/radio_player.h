#pragma once

#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class RadioPlayer {
public:
    RadioPlayer();
    ~RadioPlayer();

    // Dipanggil oleh tool "Putar Radio". TIDAK memblokir: radio baru benar-benar
    // dimulai setelah balasan suara selesai dan perangkat kembali idle.
    bool StartRadio();
    void RequestStop();

    // true selama radio menunggu giliran, sedang diputar, atau sedang dipulihkan.
    bool IsPlaying() const {
        return active_.load();
    }

private:
    static void TaskEntry(void* arg);
    static void LauncherEntry(void* arg);
    void TaskLoop();
    void LauncherLoop();
    // Memulai radio sekarang (perangkat sudah idle). true bila task radio dibuat.
    bool StartNow();

    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> active_{false};
    std::atomic<bool> pending_{false};

    // 0 = menunggu audio pertama
    // 1 = PCM berhasil dikirim
    // -1 = gagal memulai radio
    std::atomic<int> startup_state_{0};

    TaskHandle_t task_handle_ = nullptr;
    TaskHandle_t launcher_handle_ = nullptr;
};
