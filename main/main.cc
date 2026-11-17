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
#include <esp_debug_helpers.h>
#include <esp_rom_sys.h>
#include <exception>
#include <new>
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

// Dipanggil operator new tepat saat alokasi gagal, sebelum bad_alloc dilempar.
// Stack di sini masih memuat pemanggil alokasi; dibatasi 5 kali cetak agar
// tidak membanjiri log.
static void DiagNewHandler() {
    static int printed = 0;
    if (printed < 5) {
        printed++;
        esp_rom_printf("\n!!NEW_FAIL task=%s free_int=%u largest_int=%u min_int=%u\n",
                       pcTaskGetName(nullptr),
                       (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                       (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        esp_backtrace_print(16);
    }
    throw std::bad_alloc();
}

extern "C" void app_main(void)
{
    std::set_terminate(DiagTerminateHandler);
    std::set_new_handler(DiagNewHandler);
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
