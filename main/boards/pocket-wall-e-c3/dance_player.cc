#include "dance_player.h"

#include <cstring>

#include <esp_log.h>
#include <esp_timer.h>

#include "application.h"
#include "board.h"
#include "display.h"
#include "assets/lang_config.h"
#include "audio/audio_service.h"
#include "config.h"
#include "motor_controller.h"

#define TAG "DancePlayer"

namespace {

// Pola joget: goyang kiri-kanan dua kali, lalu maju-mundur sekali, diulang sampai
// lagu selesai. Setiap gerakan berpasangan dengan arah lawannya sehingga robot kurang
// lebih kembali ke posisi awal. Jeda antar gerakan membatasi lonjakan arus motor
// (motor selalu 100% daya, lihat MOTOR_MIN_DUTY_PERCENT) yang bisa mengganggu audio.
struct Step {
    int left;
    int right;
    uint16_t ms;  // lama langkah; left == right == 0 berarti jeda
};

constexpr Step kSteps[] = {
    {-100, 100, 250}, {0, 0, 200},   // putar kiri
    {100, -100, 250}, {0, 0, 200},   // putar kanan
    {-100, 100, 250}, {0, 0, 200},   // putar kiri
    {100, -100, 250}, {0, 0, 200},   // putar kanan
    {100, 100, 250},  {0, 0, 200},   // maju
    {-100, -100, 250}, {0, 0, 200},  // mundur
};
constexpr size_t kStepCount = sizeof(kSteps) / sizeof(kSteps[0]);

constexpr int kStepTimerPeriodUs = 50 * 1000;
constexpr int kReplyStartWaitMs = 2500;   // tunggu jawaban suara AI mulai
constexpr int kReplyMaxWaitMs = 12000;    // batas tunggu jawaban AI selesai
constexpr int kPlaybackMaxWaitMs = 90000; // pengaman kalau antrean audio tidak kosong
constexpr int kTaskStackBytes = 4096;

// HSV (s = v = 255) -> RGB dengan aritmetika bilangan bulat; hue 0..255.
void HueToRgb(uint8_t hue, uint8_t* r, uint8_t* g, uint8_t* b) {
    const int region = hue / 43;                 // 0..5
    const int rise = (hue - region * 43) * 6;    // 0..252
    const int fall = 255 - rise;
    switch (region) {
        case 0:  *r = 255;  *g = rise; *b = 0;    break;
        case 1:  *r = fall; *g = 255;  *b = 0;    break;
        case 2:  *r = 0;    *g = 255;  *b = rise; break;
        case 3:  *r = 0;    *g = fall; *b = 255;  break;
        case 4:  *r = rise; *g = 0;    *b = 255;  break;
        default: *r = 255;  *g = 0;    *b = fall; break;
    }
}

}  // namespace

DancePlayer::~DancePlayer() {
    RequestStop();
}

bool DancePlayer::Start(MotorController* motor) {
    bool expected = false;
    if (!active_.compare_exchange_strong(expected, true)) {
        ESP_LOGI(TAG, "Dance already running");
        return true;
    }

    motor_ = motor;
    stop_requested_.store(false);

    if (xTaskCreate(TaskEntry, "dance_player", kTaskStackBytes, this, 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Cannot create dance task");
        active_.store(false);
        return false;
    }
    return true;
}

void DancePlayer::RequestStop() {
    if (!active_.load()) {
        return;
    }
    stop_requested_.store(true);
    StopMotors();
    // Kosongkan antrean audio supaya lagu berhenti seketika dan push yang sedang
    // menunggu di task dance terbangun.
    Application::GetInstance().GetAudioService().ResetDecoder();
}

void DancePlayer::TaskEntry(void* arg) {
    auto* self = static_cast<DancePlayer*>(arg);
    self->TaskLoop();
    self->active_.store(false);
    vTaskDelete(nullptr);
}

void DancePlayer::TimerEntry(void* arg) {
    static_cast<DancePlayer*>(arg)->OnTick();
}

void DancePlayer::StopMotors() {
    if (motor_ != nullptr) {
        motor_->Stop();
    }
}

void DancePlayer::OnTick() {
    StepMotors();
    StepLights();
}

void DancePlayer::StartLights() {
    std::lock_guard<std::mutex> lock(lights_mutex_);
    if (lights_ != nullptr) {
        return;
    }
    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = DANCE_LED_GPIO;
    strip_config.max_leds = DANCE_LED_COUNT;
    strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    strip_config.led_model = LED_MODEL_WS2812;

    led_strip_rmt_config_t rmt_config = {};
    rmt_config.resolution_hz = 10 * 1000 * 1000;  // 10 MHz, sama seperti CircularStrip

    if (led_strip_new_rmt_device(&strip_config, &rmt_config, &lights_) != ESP_OK) {
        lights_ = nullptr;
        ESP_LOGW(TAG, "Cannot create WS2812B strip on GPIO%d; no lights", DANCE_LED_GPIO);
        return;
    }
    std::memset(light_pixels_, 0, sizeof(light_pixels_));
    light_pos_ = 0;
    light_dir_ = 1;
    light_hue_ = 0;
    led_strip_clear(lights_);
}

void DancePlayer::StopLights() {
    std::lock_guard<std::mutex> lock(lights_mutex_);
    if (lights_ == nullptr) {
        return;
    }
    led_strip_clear(lights_);
    led_strip_del(lights_);
    lights_ = nullptr;
}

// "Buricak burinong": komet berwarna pelangi memantul kiri-kanan di sepanjang strip
// dengan ekor memudar, ditambah kilatan warna acak di LED mana saja tiap frame.
// Memakai array 24 byte di kelas; tanpa alokasi heap per frame.
void DancePlayer::StepLights() {
    if (stop_requested_.load()) {
        return;
    }
    std::lock_guard<std::mutex> lock(lights_mutex_);
    if (lights_ == nullptr) {
        return;
    }
    auto next_random = [this]() {
        light_rng_ = light_rng_ * 1664525U + 1013904223U;
        return light_rng_ >> 16;
    };

    // Ekor memudar (x 5/8 tiap frame).
    for (int i = 0; i < DANCE_LED_COUNT; ++i) {
        for (int c = 0; c < 3; ++c) {
            light_pixels_[i][c] = static_cast<uint8_t>(light_pixels_[i][c] * 5 / 8);
        }
    }

    // Kepala komet pada posisi sekarang, warnanya berganti sepanjang waktu.
    HueToRgb(light_hue_, &light_pixels_[light_pos_][0], &light_pixels_[light_pos_][1],
             &light_pixels_[light_pos_][2]);

    // Kilatan acak berwarna lain.
    if ((next_random() & 1U) != 0) {
        const int spark = static_cast<int>(next_random() % DANCE_LED_COUNT);
        HueToRgb(static_cast<uint8_t>(next_random()), &light_pixels_[spark][0],
                 &light_pixels_[spark][1], &light_pixels_[spark][2]);
    }

    for (int i = 0; i < DANCE_LED_COUNT; ++i) {
        led_strip_set_pixel(lights_, i, light_pixels_[i][0] * DANCE_LED_BRIGHTNESS / 255,
                            light_pixels_[i][1] * DANCE_LED_BRIGHTNESS / 255,
                            light_pixels_[i][2] * DANCE_LED_BRIGHTNESS / 255);
    }
    led_strip_refresh(lights_);

    // Gerak kiri-kanan: pantul di ujung strip dan loncat warna tiap pantulan.
    light_hue_ = static_cast<uint8_t>(light_hue_ + 9);
    light_pos_ += light_dir_;
    if (light_pos_ >= DANCE_LED_COUNT - 1) {
        light_pos_ = DANCE_LED_COUNT - 1;
        light_dir_ = -1;
        light_hue_ = static_cast<uint8_t>(light_hue_ + 60);
    } else if (light_pos_ <= 0) {
        light_pos_ = 0;
        light_dir_ = 1;
        light_hue_ = static_cast<uint8_t>(light_hue_ + 60);
    }
}

// Dipanggil dari timer tiap 50 ms; hanya memanggil Drive()/Stop() yang tidak memblokir.
void DancePlayer::StepMotors() {
    if (motor_ == nullptr || stop_requested_.load()) {
        return;
    }
    const int64_t now_ms = (esp_timer_get_time() - dance_start_us_) / 1000;
    // Batasi loop kejar-ketinggalan supaya callback timer tetap singkat.
    for (int guard = 0; guard < 4 && now_ms >= next_change_ms_; ++guard) {
        const Step& step = kSteps[step_index_ % kStepCount];
        if (step.left == 0 && step.right == 0) {
            motor_->Stop();
        } else {
            motor_->Drive(step.left, step.right, step.ms);
        }
        next_change_ms_ += step.ms;
        ++step_index_;
    }
}

// Kalau tool dipanggil dari sesi chat, AI biasanya menjawab dengan suara dulu. Tunggu
// jawaban itu selesai supaya tidak bertabrakan dengan lagu di antrean audio yang sama.
void DancePlayer::WaitForAssistantReply() {
    auto& app = Application::GetInstance();
    auto& audio = app.GetAudioService();

    const int64_t t0 = esp_timer_get_time();
    bool seen_speaking = false;
    while (!stop_requested_.load()) {
        const DeviceState state = app.GetDeviceState();
        if (state == kDeviceStateIdle) {
            break;  // tidak ada sesi chat, tidak ada yang perlu ditunggu
        }
        const int elapsed_ms = static_cast<int>((esp_timer_get_time() - t0) / 1000);
        if (state == kDeviceStateSpeaking) {
            seen_speaking = true;
        } else if (seen_speaking && audio.IsPlaybackIdle()) {
            break;  // jawaban AI selesai
        } else if (!seen_speaking && elapsed_ms >= kReplyStartWaitMs) {
            break;  // AI tidak menjawab dengan suara
        }
        if (elapsed_ms >= kReplyMaxWaitMs) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// Tutup sesi chat supaya mikrofon mati (tidak mengirim musik/bising motor ke server)
// dan suara TTS susulan tidak tercampur ke lagu.
void DancePlayer::CloseChatSession() {
    auto& app = Application::GetInstance();
    for (int i = 0; i < 5 && !stop_requested_.load(); ++i) {
        const DeviceState state = app.GetDeviceState();
        if (state != kDeviceStateListening && state != kDeviceStateSpeaking) {
            break;
        }
        app.ToggleChatState();  // Listening: tutup channel; Speaking: hentikan jawaban
        vTaskDelay(pdMS_TO_TICKS(400));
    }
}

void DancePlayer::TaskLoop() {
    auto& app = Application::GetInstance();
    auto& audio = app.GetAudioService();

    // Beri waktu hasil tool terkirim ke server sebelum mengubah sesi.
    vTaskDelay(pdMS_TO_TICKS(300));
    WaitForAssistantReply();
    CloseChatSession();
    if (stop_requested_.load()) {
        return;
    }

    ESP_LOGI(TAG, "Joget dimulai");

    StartLights();

    if (motor_ != nullptr || lights_ != nullptr) {
        dance_start_us_ = esp_timer_get_time();
        next_change_ms_ = 0;
        step_index_ = 0;
        const esp_timer_create_args_t args = {
            .callback = &DancePlayer::TimerEntry,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "dance_step",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &timer_) == ESP_OK) {
            esp_timer_start_periodic(timer_, kStepTimerPeriodUs);
        } else {
            timer_ = nullptr;
            ESP_LOGW(TAG, "Cannot create step timer; music only");
        }
    }

    // Layar joget (bintang, equalizer, wajah goyang); dihapus lagi di akhir.
    auto* display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->SetStatus("DANCE");
    }

    // Memblokir sampai semua paket lagu masuk antrean (laju mengikuti pemutaran).
    audio.PlaySound(Lang::Sounds::OGG_JOGET, [this]() { return stop_requested_.load(); });

    // Semua paket sudah di antrean; tunggu sisa lagu selesai diputar.
    const int64_t wait_start = esp_timer_get_time();
    while (!stop_requested_.load() && !audio.IsPlaybackIdle()) {
        if ((esp_timer_get_time() - wait_start) / 1000 > kPlaybackMaxWaitMs) {
            ESP_LOGW(TAG, "Timeout menunggu lagu selesai");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (timer_ != nullptr) {
        esp_timer_stop(timer_);
        esp_timer_delete(timer_);
        timer_ = nullptr;
    }
    StopMotors();
    StopLights();
    if (display != nullptr) {
        display->SetStatus("DANCE_END");
    }
    if (stop_requested_.load()) {
        // Buang paket yang sempat lolos sebelum flag stop terbaca.
        audio.ResetDecoder();
    }
    ESP_LOGI(TAG, "Joget selesai%s", stop_requested_.load() ? " (dihentikan)" : "");
}
