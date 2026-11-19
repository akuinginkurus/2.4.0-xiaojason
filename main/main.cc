#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_debug_helpers.h>
#include <esp_rom_sys.h>
#include <exception>
#include <new>
#include <cstdint>
#include <cstdlib>
#include "application.h"

#define TAG "main"

// Diagnostik crash: dicetak lewat ROM UART (sinkron) sebelum abort() supaya
// tidak ikut tertimpa log lain di serial monitor. Stack di sini masih memuat
// frame fungsi yang melempar exception, jadi backtrace menunjukkan pelakunya.
static void DiagTerminateHandler() {
    const char* task = pcTaskGetName(nullptr);
    const char* what = "unknown";
    try {
        if (auto ep = std::current_exception()) {
            std::rethrow_exception(ep);
        }
    } catch (const std::exception& e) {
        what = e.what();
    } catch (...) {
    }
    esp_rom_printf("\n!!TERMINATE task=%s what=%s free_int=%u largest_int=%u min_int=%u\n",
                   task ? task : "?", what,
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                   (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    esp_backtrace_print(32);
    abort();
}

// Pengganti operator new global: bila alokasi gagal, cetak ukuran yang diminta
// dan alamat pemanggilnya (resolve dengan addr2line + .elf) sebelum melempar
// bad_alloc. Dibatasi 40 cetakan agar log tidak banjir.
__attribute__((noinline)) void* operator new(std::size_t size) {
    void* ptr = std::malloc(size == 0 ? 1 : size);
    if (ptr == nullptr) {
        static int printed = 0;
        if (printed < 40) {
            printed++;
            esp_rom_printf(
                "\n!!NEW_FAIL size=%u caller=0x%08x task=%s free_int=%u largest_int=%u\n",
                (unsigned)size,
                (unsigned)(uintptr_t)__builtin_return_address(0),
                pcTaskGetName(nullptr),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
        throw std::bad_alloc();
    }
    return ptr;
}

// POWERON/BROWNOUT = masalah catu daya; PANIC = crash kode;
// INT_WDT/TASK_WDT/WDT = watchdog; SW_RESTART = esp_restart().
static void PrintResetReason() {
    const char* reset_name = "LAIN";
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON: reset_name = "POWERON"; break;
        case ESP_RST_SW: reset_name = "SW_RESTART"; break;
        case ESP_RST_PANIC: reset_name = "PANIC"; break;
        case ESP_RST_INT_WDT: reset_name = "INT_WDT"; break;
        case ESP_RST_TASK_WDT: reset_name = "TASK_WDT"; break;
        case ESP_RST_WDT: reset_name = "WDT"; break;
        case ESP_RST_BROWNOUT: reset_name = "BROWNOUT"; break;
        case ESP_RST_USB: reset_name = "USB"; break;
        case ESP_RST_JTAG: reset_name = "JTAG"; break;
        case ESP_RST_EFUSE: reset_name = "EFUSE"; break;
        default: break;
    }
    esp_rom_printf("\n!!RESET_REASON %s (%d) uptime=%lld ms\n", reset_name,
                   (int)esp_reset_reason(), (long long)(esp_timer_get_time() / 1000));
}

extern "C" void app_main(void)
{
    std::set_terminate(DiagTerminateHandler);

    // Catat penyebab reset terakhir. Dicetak saat boot dan diulang tiap 30 detik
    // selama 5 menit pertama, karena USB serial terputus saat reboot dan monitor
    // sering baru tersambung beberapa menit kemudian.
    PrintResetReason();
    static esp_timer_handle_t reset_reason_timer = nullptr;
    esp_timer_create_args_t reset_timer_args = {};
    reset_timer_args.callback = [](void*) {
        static int printed = 0;
        PrintResetReason();
        if (++printed >= 10 && reset_reason_timer != nullptr) {
            esp_timer_stop(reset_reason_timer);
        }
    };
    reset_timer_args.name = "reset_reason";
    if (esp_timer_create(&reset_timer_args, &reset_reason_timer) == ESP_OK) {
        esp_timer_start_periodic(reset_reason_timer, 30ULL * 1000 * 1000);
    }
    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize and run the application
    auto& app = Application::GetInstance();
    app.Initialize();
    app.Run();  // This function runs the main event loop and never returns
}
