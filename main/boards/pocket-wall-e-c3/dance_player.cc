#include "dance_player.h"

#include <cstring>

#include <esp_log.h>
#include <esp_timer.h>

#include "application.h"
#include "assets/lang_config.h"
#include "audio/audio_service.h"
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
    static_cast<DancePlayer*>(arg)->StepMotors();
}

void DancePlayer::StopMotors() {
    if (motor_ != nullptr) {
        motor_->Stop();
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

    if (motor_ != nullptr) {
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
    if (stop_requested_.load()) {
        // Buang paket yang sempat lolos sebelum flag stop terbaca.
        audio.ResetDecoder();
    }
    ESP_LOGI(TAG, "Joget selesai%s", stop_requested_.load() ? " (dihentikan)" : "");
}
