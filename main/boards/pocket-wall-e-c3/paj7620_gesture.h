#ifndef PAJ7620_GESTURE_H
#define PAJ7620_GESTURE_H

// Pembaca gestur tangan PAJ7620U2 (alamat I2C 0x73) untuk ESP-IDF.
// Tabel register diambil dari library RevEng_PAJ7620 (nilai dari dokumentasi
// PixArt v0.8/v1.0). Hemat RAM: tanpa task baru (satu esp_timer 100 ms),
// tabel register disimpan sebagai const di flash, tanpa alokasi heap.
//
// Wiring: VCC=3V3, GND, SDA=GPIO4, SCL=GPIO5 (bus I2C yang sama dengan OLED).
// Pin INT tidak dipakai (gestur dibaca dengan polling).

#include <cstddef>
#include <cstdint>

#include <driver/i2c_master.h>
#include <esp_log.h>
#include <esp_timer.h>

enum PajGesture : uint8_t {
    kPajGestureUp = 1,
    kPajGestureDown,
    kPajGestureLeft,
    kPajGestureRight,
    kPajGestureForward,
    kPajGestureBackward,
    kPajGestureClockwise,
    kPajGestureAnticlockwise,
    kPajGestureWave,
};

class Paj7620Gesture {
public:
    // Callback dipanggil dari task esp_timer. Jangan lakukan kerja berat di
    // dalamnya; teruskan ke Application::Schedule().
    using Callback = void (*)(PajGesture gesture, void* context);

    Paj7620Gesture(i2c_master_bus_handle_t bus, Callback callback, void* context)
        : callback_(callback), context_(context) {
        if (bus == nullptr) {
            return;
        }

        // Sensor tidur saat bus sepi dan sering melewatkan pesan pertama:
        // probe dulu untuk membangunkannya.
        i2c_master_probe(bus, kAddress, 50);
        if (i2c_master_probe(bus, kAddress, 50) != ESP_OK) {
            ESP_LOGW(kTag, "PAJ7620U2 tidak ditemukan di 0x%02X (cek kabel)", kAddress);
            return;
        }

        i2c_device_config_t device_config = {};
        device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        device_config.device_address = kAddress;
        device_config.scl_speed_hz = 400 * 1000;

        if (i2c_master_bus_add_device(bus, &device_config, &device_) != ESP_OK) {
            ESP_LOGW(kTag, "Gagal menambah perangkat PAJ7620U2 ke bus I2C");
            device_ = nullptr;
            return;
        }

        // Pilih bank 0 dua kali (sesuai library: pesan pertama sering hilang).
        WriteReg(0xEF, 0x00);
        WriteReg(0xEF, 0x00);

        uint8_t id0 = 0, id1 = 0;
        if (!ReadReg(0x00, &id0) || !ReadReg(0x01, &id1) || id0 != 0x20 || id1 != 0x76) {
            ESP_LOGW(kTag, "ID sensor tidak cocok (0x%02X 0x%02X), bukan PAJ7620U2", id0, id1);
            return;
        }

        WriteTable(kInitRegisters, sizeof(kInitRegisters) / sizeof(kInitRegisters[0]));
        WriteTable(kGestureModeRegisters,
                   sizeof(kGestureModeRegisters) / sizeof(kGestureModeRegisters[0]));
        WriteReg(0xEF, 0x00);  // parkir di bank 0

        esp_timer_create_args_t args = {};
        args.callback = &Paj7620Gesture::TimerCallback;
        args.arg = this;
        args.name = "paj7620";
        if (esp_timer_create(&args, &timer_) != ESP_OK) {
            ESP_LOGW(kTag, "Gagal membuat timer PAJ7620U2");
            return;
        }
        esp_timer_start_periodic(timer_, static_cast<uint64_t>(kPollMs) * 1000);

        ready_ = true;
        ESP_LOGI(kTag, "PAJ7620U2 siap");
    }

    ~Paj7620Gesture() {
        if (timer_ != nullptr) {
            esp_timer_stop(timer_);
            esp_timer_delete(timer_);
        }
    }

    bool ready() const { return ready_; }

private:
    static constexpr const char* kTag = "Paj7620Gesture";
    static constexpr uint8_t kAddress = 0x73;
    static constexpr int kPollMs = 100;
    static constexpr int64_t kCooldownMs = 900;  // abaikan gestur ikutan setelah satu gestur

    // Tabel register dari library RevEng_PAJ7620: byte atas = alamat, byte bawah = nilai.
    static constexpr uint16_t kInitRegisters[] = {
    0xEF00, 0x4100, 0x4200, 0x3707, 0x3817, 0x3906, 0x4201, 0x462D,
    0x470F, 0x483C, 0x4900, 0x4A1E, 0x4C22, 0x5110, 0x5E10, 0x6027,
    0x8042, 0x8144, 0x8204, 0x8B01, 0x9006, 0x950A, 0x960C, 0x9705,
    0x9A14, 0x9C3F, 0xA519, 0xCC19, 0xCD0B, 0xCE13, 0xCF64, 0xD021,
    0xEF01, 0x020F, 0x0310, 0x0402, 0x2501, 0x2739, 0x287F, 0x2908,
    0x3EFF, 0x5E3D, 0x6596, 0x6797, 0x69CD, 0x6A01, 0x6D2C, 0x6E01,
    0x7201, 0x7335, 0x7400, 0x7701, 0xEF00, 0x41FF, 0x4201,
    };

    static constexpr uint16_t kGestureModeRegisters[] = {
    0xEF00, 0x4100, 0x4200, 0x483C, 0x4900, 0x5110, 0x8320, 0x9ff9,
    0xEF01, 0x011E, 0x020F, 0x0310, 0x0402, 0x4140, 0x4330, 0x6596,
    0x6600, 0x6797, 0x6801, 0x69CD, 0x6A01, 0x6bb0, 0x6c04, 0x6D2C,
    0x6E01, 0x7400, 0xEF00, 0x41FF, 0x4201,
    };

    i2c_master_dev_handle_t device_ = nullptr;
    esp_timer_handle_t timer_ = nullptr;
    Callback callback_ = nullptr;
    void* context_ = nullptr;
    bool ready_ = false;
    int64_t last_gesture_ms_ = 0;

    static int64_t NowMs() { return esp_timer_get_time() / 1000; }

    static void TimerCallback(void* arg) {
        static_cast<Paj7620Gesture*>(arg)->Poll();
    }

    bool WriteReg(uint8_t reg, uint8_t value) {
        const uint8_t buffer[2] = {reg, value};
        return i2c_master_transmit(device_, buffer, 2, 30) == ESP_OK;
    }

    bool ReadReg(uint8_t reg, uint8_t* value) {
        return i2c_master_transmit_receive(device_, &reg, 1, value, 1, 30) == ESP_OK;
    }

    void WriteTable(const uint16_t* table, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            WriteReg(static_cast<uint8_t>(table[i] >> 8),
                     static_cast<uint8_t>(table[i] & 0xFF));
        }
    }

    void Poll() {
        uint8_t flags = 0;
        uint8_t wave = 0;
        if (!ReadReg(0x43, &flags)) {
            return;
        }
        ReadReg(0x44, &wave);

        if (flags == 0 && (wave & 0x01) == 0) {
            return;
        }

        const int64_t now = NowMs();
        if (now - last_gesture_ms_ < kCooldownMs) {
            return;  // flag sudah terbaca (dan terhapus), abaikan gestur ikutan
        }

        PajGesture gesture;
        if (flags & 0x01) {
            gesture = kPajGestureUp;
        } else if (flags & 0x02) {
            gesture = kPajGestureDown;
        } else if (flags & 0x04) {
            gesture = kPajGestureLeft;
        } else if (flags & 0x08) {
            gesture = kPajGestureRight;
        } else if (flags & 0x10) {
            gesture = kPajGestureForward;
        } else if (flags & 0x20) {
            gesture = kPajGestureBackward;
        } else if (flags & 0x40) {
            gesture = kPajGestureClockwise;
        } else if (flags & 0x80) {
            gesture = kPajGestureAnticlockwise;
        } else {
            gesture = kPajGestureWave;
        }

        last_gesture_ms_ = now;
        ESP_LOGI(kTag, "Gestur terdeteksi: %d", static_cast<int>(gesture));
        if (callback_ != nullptr) {
            callback_(gesture, context_);
        }
    }
};

#endif  // PAJ7620_GESTURE_H
