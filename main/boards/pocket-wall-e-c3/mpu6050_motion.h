#ifndef MPU6050_MOTION_H
#define MPU6050_MOTION_H

// Deteksi "diangkat" dan "digoyang" memakai MPU6050 (hanya akselerometer).
// Hemat RAM: tanpa task baru (pakai satu esp_timer), tanpa buffer besar,
// tanpa alokasi saat membaca. Hasilnya meminta wajah marah lewat
// PocketOledRequestAngry() yang didefinisikan di pocket_oled_display.cc.
//
// Wiring: VCC=3V3, GND, SDA=GPIO4, SCL=GPIO5 (bus I2C yang sama dengan OLED),
// AD0=GND (alamat 0x68). Pin INT tidak dipakai.

#include <atomic>
#include <cstdint>
#include <cstdlib>

#include <driver/i2c_master.h>
#include <esp_log.h>
#include <esp_timer.h>

// Didefinisikan di pocket_oled_display.cc.
// kind: 1 = diangkat, 2 = digoyang (menentukan teks di layar).
extern void PocketOledRequestAngry(uint32_t hold_ms, int kind);

class Mpu6050Motion {
public:
    explicit Mpu6050Motion(i2c_master_bus_handle_t bus) {
        if (bus == nullptr) {
            return;
        }

        if (i2c_master_probe(bus, kAddress, 50) != ESP_OK) {
            ESP_LOGW(kTag, "MPU6050 tidak ditemukan di 0x%02X (cek kabel dan AD0=GND)",
                     kAddress);
            return;
        }

        i2c_device_config_t device_config = {};
        device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        device_config.device_address = kAddress;
        device_config.scl_speed_hz = 400 * 1000;

        if (i2c_master_bus_add_device(bus, &device_config, &device_) != ESP_OK) {
            ESP_LOGW(kTag, "Gagal menambah perangkat MPU6050 ke bus I2C");
            device_ = nullptr;
            return;
        }

        uint8_t who = 0;
        const uint8_t who_reg = 0x75;
        if (i2c_master_transmit_receive(device_, &who_reg, 1, &who, 1, 50) == ESP_OK) {
            ESP_LOGI(kTag, "WHO_AM_I = 0x%02X", who);
        }

        // Bangunkan sensor (clock PLL gyro X), filter 44 Hz, rentang +-4 g.
        if (!WriteReg(0x6B, 0x01) || !WriteReg(0x1A, 0x03) || !WriteReg(0x1C, 0x08)) {
            ESP_LOGW(kTag, "Gagal mengonfigurasi MPU6050");
            return;
        }

        esp_timer_create_args_t args = {};
        args.callback = &Mpu6050Motion::TimerCallback;
        args.arg = this;
        args.name = "mpu6050";
        if (esp_timer_create(&args, &timer_) != ESP_OK) {
            ESP_LOGW(kTag, "Gagal membuat timer MPU6050");
            return;
        }
        esp_timer_start_periodic(timer_, static_cast<uint64_t>(kPollMs) * 1000);

        ready_ = true;
        ESP_LOGI(kTag, "MPU6050 siap (diangkat/digoyang -> marah)");
    }

    ~Mpu6050Motion() {
        if (timer_ != nullptr) {
            esp_timer_stop(timer_);
            esp_timer_delete(timer_);
        }
    }

    bool ready() const { return ready_; }

    // Pasang pemeriksa "robot sedang bergerak" (mis. MotorController::IsMoving).
    // Selama motor jalan, sensor gerak diabaikan agar getaran motor tidak
    // dikira diangkat / digoyang.
    void SetMotionProbe(bool (*probe)(void*), void* context) {
        probe_ctx_ = context;
        probe_ = probe;
    }

    // Dipanggil sekali tiap kali "diangkat" / "digoyang" terdeteksi
    // (kind 1 = diangkat, 2 = digoyang), dari task esp_timer.
    void SetReactCallback(void (*callback)(int kind, void* context), void* context) {
        react_ctx_ = context;
        react_ = callback;
    }

    // Abaikan gerakan selama ms milidetik (mis. saat robot sendiri bergerak).
    void SuppressFor(uint32_t ms) {
        suppress_until_ms_.store(NowMs() + static_cast<int64_t>(ms));
    }

private:
    static constexpr const char* kTag = "Mpu6050Motion";
    static constexpr uint8_t kAddress = 0x68;

    // Waktu dan ambang. Ubah angka ini bila terlalu sensitif / kurang peka.
    static constexpr int kPollMs = 40;                  // 25 kali per detik
    static constexpr int32_t kShakeDeltaMg = 900;       // loncatan antar sampel
    static constexpr int kShakeJerkCount = 4;           // jumlah loncatan
    static constexpr int64_t kShakeWindowMs = 1200;     // dalam jendela ini
    static constexpr int32_t kLiftMagDevMg = 250;       // |a| menyimpang dari 1 g
    static constexpr int32_t kCosTiltPermille = 866;    // cos 30 derajat
    static constexpr int kLiftConfirmSamples = 5;       // 5 x 40 ms = 200 ms
    static constexpr int32_t kCalmDeltaMg = 120;
    static constexpr int32_t kCalmDevMg = 120;
    static constexpr int64_t kStableAdoptMs = 2000;     // diam -> baseline baru
    static constexpr uint32_t kAngryHoldLiftMs = 3000;
    static constexpr uint32_t kAngryHoldShakeMs = 3000;

    i2c_master_dev_handle_t device_ = nullptr;
    esp_timer_handle_t timer_ = nullptr;
    bool ready_ = false;
    int fail_count_ = 0;

    std::atomic<int64_t> suppress_until_ms_{0};

    bool (*probe_)(void*) = nullptr;
    void* probe_ctx_ = nullptr;
    void (*react_)(int, void*) = nullptr;
    void* react_ctx_ = nullptr;
    static constexpr int64_t kMotorSuppressMs = 1500;  // sisa getaran setelah motor berhenti

    bool has_prev_ = false;
    int32_t px_ = 0, py_ = 0, pz_ = 0;
    int32_t bx_ = 0, by_ = 0, bz_ = 0, bmag_ = 1000;

    int jerk_count_ = 0;
    int64_t jerk_first_ms_ = 0;
    int moving_samples_ = 0;
    int64_t stable_since_ms_ = 0;
    int64_t last_request_ms_ = 0;
    bool lifted_ = false;

    static int64_t NowMs() { return esp_timer_get_time() / 1000; }

    static void TimerCallback(void* arg) {
        static_cast<Mpu6050Motion*>(arg)->Poll();
    }

    static uint32_t Isqrt(uint32_t value) {
        uint32_t result = 0;
        uint32_t bit = 1u << 30;
        while (bit > value) {
            bit >>= 2;
        }
        while (bit != 0) {
            if (value >= result + bit) {
                value -= result + bit;
                result = (result >> 1) + bit;
            } else {
                result >>= 1;
            }
            bit >>= 2;
        }
        return result;
    }

    // Rentang +-4 g: 8192 LSB per g  ->  mg = raw * 1000 / 8192.
    static int32_t ToMg(uint8_t high, uint8_t low) {
        const int16_t raw = static_cast<int16_t>((high << 8) | low);
        return static_cast<int32_t>(raw) * 125 / 1024;
    }

    bool WriteReg(uint8_t reg, uint8_t value) {
        const uint8_t buffer[2] = {reg, value};
        return i2c_master_transmit(device_, buffer, 2, 50) == ESP_OK;
    }

    void ResetBaseline(int32_t x, int32_t y, int32_t z, int32_t mag) {
        bx_ = x;
        by_ = y;
        bz_ = z;
        bmag_ = mag;
    }

    void ClearTracking() {
        jerk_count_ = 0;
        moving_samples_ = 0;
        stable_since_ms_ = 0;
        lifted_ = false;
    }

    void Trigger(const char* reason, uint32_t hold_ms, int kind, int64_t now) {
        ESP_LOGI(kTag, "Terdeteksi: %s -> marah", reason);
        PocketOledRequestAngry(hold_ms, kind);
        last_request_ms_ = now;
        if (react_ != nullptr) {
            react_(kind, react_ctx_);
        }
    }

    void Poll() {
        // Satu pembacaan = 6 byte di stack, tidak ada alokasi heap.
        const uint8_t reg = 0x3B;
        uint8_t data[6];
        if (i2c_master_transmit_receive(device_, &reg, 1, data, 6, 20) != ESP_OK) {
            if (++fail_count_ == 50) {
                ESP_LOGW(kTag, "Pembacaan MPU6050 gagal berulang kali");
            }
            return;
        }
        fail_count_ = 0;

        const int32_t x = ToMg(data[0], data[1]);
        const int32_t y = ToMg(data[2], data[3]);
        const int32_t z = ToMg(data[4], data[5]);
        const int32_t mag = static_cast<int32_t>(
            Isqrt(static_cast<uint32_t>(x * x + y * y + z * z)));
        const int64_t now = NowMs();

        if (!has_prev_) {
            has_prev_ = true;
            px_ = x;
            py_ = y;
            pz_ = z;
            ResetBaseline(x, y, z, mag);
            return;
        }

        const int32_t delta =
            std::abs(x - px_) + std::abs(y - py_) + std::abs(z - pz_);
        px_ = x;
        py_ = y;
        pz_ = z;

        if (probe_ != nullptr && probe_(probe_ctx_)) {
            suppress_until_ms_.store(now + kMotorSuppressMs);
        }

        if (now < suppress_until_ms_.load()) {
            ResetBaseline(x, y, z, mag);
            ClearTracking();
            return;
        }

        // 1) Digoyang: beberapa loncatan besar dalam jendela waktu singkat.
        if (delta > kShakeDeltaMg) {
            if (jerk_count_ == 0 || now - jerk_first_ms_ > kShakeWindowMs) {
                jerk_first_ms_ = now;
                jerk_count_ = 0;
            }
            if (++jerk_count_ >= kShakeJerkCount) {
                Trigger("digoyang", kAngryHoldShakeMs, 2, now);
                jerk_count_ = 0;
                lifted_ = true;  // jangan memicu "diangkat" lagi untuk gerakan yang sama
            }
        }

        // 2) Diangkat: besar percepatan menyimpang dari 1 g, atau arah gravitasi
        //    berubah > 30 derajat dari posisi diam, bertahan >= 200 ms.
        const int32_t deviation = std::abs(mag - 1000);
        bool tilted = false;
        if (mag > 0 && bmag_ > 0) {
            const int64_t dot = static_cast<int64_t>(x) * bx_ +
                                static_cast<int64_t>(y) * by_ +
                                static_cast<int64_t>(z) * bz_;
            tilted = dot * 1000 <
                     static_cast<int64_t>(kCosTiltPermille) * mag * bmag_;
        }

        const bool moving = deviation > kLiftMagDevMg || tilted;
        const bool calm = delta < kCalmDeltaMg && deviation < kCalmDevMg;

        if (moving) {
            ++moving_samples_;
        } else {
            moving_samples_ = 0;
        }

        // Diam cukup lama -> posisi sekarang jadi baseline baru.
        if (calm) {
            if (stable_since_ms_ == 0) {
                stable_since_ms_ = now;
            } else if (now - stable_since_ms_ >= kStableAdoptMs) {
                ResetBaseline(x, y, z, mag);
                lifted_ = false;
            }
        } else {
            stable_since_ms_ = 0;
        }

        if (moving_samples_ >= kLiftConfirmSamples) {
            if (!lifted_) {
                lifted_ = true;
                Trigger("diangkat", kAngryHoldLiftMs, 1, now);
            } else if (!calm && now - last_request_ms_ > 1000) {
                // Masih dibawa-bawa: perpanjang marah.
                PocketOledRequestAngry(kAngryHoldLiftMs / 2, 1);
                last_request_ms_ = now;
            }
        }
    }
};

#endif  // MPU6050_MOTION_H
