#pragma once

#include <atomic>
#include <string>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class RadioPlayer {
public:
    explicit RadioPlayer();
    ~RadioPlayer();

    bool StartPrambors();
    void RequestStop();
    bool IsPlaying() const { return active_.load(); }

private:
    static void TaskEntry(void* arg);
    void TaskLoop();

    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> active_{false};
    TaskHandle_t task_handle_ = nullptr;
};
