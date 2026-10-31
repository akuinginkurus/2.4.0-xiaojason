#include "radio_player.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <strings.h>
#include <vector>
#include <utility>
#include "board.h"
#include "display.h"
#include "assets/lang_config.h"
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_crt_bundle.h>
#include <esp_audio_simple_dec.h>
#include <esp_audio_simple_dec_default.h>

#include "application.h"
#include "audio/audio_service.h"

#define TAG "RadioPlayer"

namespace {

// Daftar stasiun, dicoba berurutan sampai ada yang berhasil tersambung.
// Semua harus MP3. Utamakan http:// (tanpa TLS) karena handshake TLS
// membutuhkan RAM besar di ESP32-C3 dan sebelumnya gagal.
struct Station {
    const char* name;
    const char* url;
};

constexpr Station kStations[] = {
    // MP3, http (tanpa TLS)
    {"I-Radio Jakarta",     "http://stream.radiojar.com/4ywdgup3bnzuv"},
    {"Hard Rock FM 87.6",   "http://stream.radiojar.com/7csmg90fuqruv"},
    // MP3 192 kbps, paling berat, hanya cadangan
    {"Radio Dangdut 97.1",  "http://202.147.199.99:8000/;"},
    // MP3 64 kbps, https: cadangan terakhir
    {"Elshinta FM",         "https://stream-ssl.arenastreaming.com:8000/jakarta"},
};
constexpr size_t kStationCount = sizeof(kStations) / sizeof(kStations[0]);

constexpr size_t kHttpReadSize = 1024;
// Satu frame MP3 stereo 16-bit = 4608 byte.
constexpr size_t kDecoderOutputSize = 4608;
constexpr int kRadioOutputRate = 24000;
constexpr int kMaxRedirects = 5;
constexpr int kConnectTimeoutMs = 8000;
constexpr int kStartupTimeoutMs = 30000;

struct HttpResponseHeaders {
    std::string content_type;
};

esp_err_t HttpEventHandler(esp_http_client_event_t* event) {
    if (event == nullptr || event->user_data == nullptr) {
        return ESP_OK;
    }

    if (event->event_id == HTTP_EVENT_ON_HEADER &&
        event->header_key != nullptr &&
        event->header_value != nullptr) {
        auto* headers = static_cast<HttpResponseHeaders*>(event->user_data);
        if (strcasecmp(event->header_key, "Content-Type") == 0) {
            headers->content_type = event->header_value;
        }
    }

    return ESP_OK;
}

bool IsMp3ContentType(const std::string& content_type) {
    std::string value = content_type;
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        }
    );

    return value.find("audio/mpeg") != std::string::npos ||
           value.find("audio/mp3") != std::string::npos ||
           value.find("audio/x-mpeg") != std::string::npos ||
           value.find("application/octet-stream") != std::string::npos;
}

// Membuka satu stasiun: ikuti redirect, cek status 200/206 dan Content-Type MP3.
// Berhasil: mengembalikan handle HTTP yang siap dibaca.
// Gagal: semua resource dibersihkan dan mengembalikan nullptr.
esp_http_client_handle_t ConnectStation(
    const Station& station,
    HttpResponseHeaders& headers,
    const std::atomic<bool>& stop_requested) {

    esp_http_client_config_t http_cfg = {};
    http_cfg.url = station.url;
    http_cfg.timeout_ms = kConnectTimeoutMs;
    http_cfg.buffer_size = 2048;
    http_cfg.buffer_size_tx = 512;
    // Redirect ditangani manual.
    http_cfg.disable_auto_redirect = true;
    http_cfg.max_redirection_count = kMaxRedirects;
    http_cfg.event_handler = HttpEventHandler;
    http_cfg.user_data = &headers;
    // Hanya dipakai kalau URL atau redirect-nya https.
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == nullptr) {
        ESP_LOGE(TAG, "Cannot init HTTP client");
        return nullptr;
    }

    bool connected = false;

    for (int redirect = 0; redirect <= kMaxRedirects; redirect++) {
        if (stop_requested.load()) {
            break;
        }

        headers.content_type.clear();

        const esp_err_t open_err = esp_http_client_open(client, 0);
        if (open_err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(open_err));
            break;
        }

        const int64_t content_length = esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP status: %d, length=%lld", status,
                 static_cast<long long>(content_length));

        if (status == 301 || status == 302 || status == 303 ||
            status == 307 || status == 308) {
            ESP_LOGW(TAG, "Radio redirect %d/%d", redirect + 1, kMaxRedirects);

            if (redirect >= kMaxRedirects) {
                ESP_LOGE(TAG, "Too many radio redirects");
                esp_http_client_close(client);
                break;
            }

            const esp_err_t redirect_err = esp_http_client_set_redirection(client);
            esp_http_client_close(client);

            if (redirect_err != ESP_OK) {
                ESP_LOGE(TAG, "Cannot follow redirect: %s",
                         esp_err_to_name(redirect_err));
                break;
            }
            continue;
        }

        if (status == 200 || status == 206) {
            connected = true;
        } else {
            ESP_LOGE(TAG, "Radio HTTP status error: %d", status);
            esp_http_client_close(client);
        }
        break;
    }

    if (!connected) {
        esp_http_client_cleanup(client);
        return nullptr;
    }

    ESP_LOGI(TAG, "Radio Content-Type: %s",
             headers.content_type.empty() ? "(empty)" : headers.content_type.c_str());

    if (!IsMp3ContentType(headers.content_type)) {
        ESP_LOGE(TAG, "Unsupported radio Content-Type, skipping station");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return nullptr;
    }

    return client;
}

// Resampler linear tanpa alokasi heap (pengganti esp_ae_rate_cvt yang butuh
// blok 10 KB berurutan dan gagal di ESP32-C3).
struct LinearResampler {
    uint32_t src_rate = 0;
    uint32_t step = 0;   // 16.16 fixed point
    uint32_t pos = 0;
    int16_t prev = 0;
    bool primed = false;

    void Configure(uint32_t src, uint32_t dst) {
        if (src == src_rate) {
            return;
        }
        src_rate = src;
        step = static_cast<uint32_t>((static_cast<uint64_t>(src) << 16) / dst);
        pos = 0;
        primed = false;
    }

    void Process(const int16_t* in, size_t n, std::vector<int16_t>& out) {
        out.clear();
        if (n == 0 || src_rate == 0) {
            return;
        }
        if (!primed) {
            prev = in[0];
            primed = true;
        }
        out.reserve(n * kRadioOutputRate / src_rate + 2);
        while ((pos >> 16) < n) {
            const size_t idx = pos >> 16;
            const int32_t a = (idx == 0) ? prev : in[idx - 1];
            const int32_t b = in[idx];
            const int64_t frac = pos & 0xFFFF;
            out.push_back(static_cast<int16_t>(
                a + (((static_cast<int64_t>(b) - a) * frac) >> 16)));
            pos += step;
        }
        pos -= static_cast<uint32_t>(n) << 16;
        prev = in[n - 1];
    }
};

bool ConvertAndQueue(
    AudioService& audio,
    const uint8_t* pcm_bytes,
    size_t byte_count,
    const esp_audio_simple_dec_info_t& info,
    LinearResampler& resampler) {

    if (info.bits_per_sample != 16 ||
        info.channel == 0 ||
        info.channel > 2 ||
        info.sample_rate == 0 ||
        byte_count < sizeof(int16_t)) {
        ESP_LOGE(
            TAG,
            "Unsupported PCM: %u bit, %u ch, %lu Hz",
            static_cast<unsigned>(info.bits_per_sample),
            static_cast<unsigned>(info.channel),
            static_cast<unsigned long>(info.sample_rate)
        );
        return false;
    }

    const auto* input = reinterpret_cast<const int16_t*>(pcm_bytes);
    const size_t input_samples = byte_count / sizeof(int16_t);

    std::vector<int16_t> mono;

    // Stereo menjadi mono.
    if (info.channel == 1) {
        mono.assign(input, input + input_samples);
    } else {
        const size_t frames = input_samples / 2;
        mono.resize(frames);

        for (size_t i = 0; i < frames; i++) {
            const int32_t mixed =
                static_cast<int32_t>(input[i * 2]) +
                static_cast<int32_t>(input[i * 2 + 1]);
            mono[i] = static_cast<int16_t>(mixed / 2);
        }
    }

    if (mono.empty()) {
        return true;
    }

    // Resampling ke 24 kHz tanpa alokasi heap besar.
    if (info.sample_rate != kRadioOutputRate) {
        resampler.Configure(info.sample_rate, kRadioOutputRate);
        std::vector<int16_t> converted;
        resampler.Process(mono.data(), mono.size(), converted);
        mono = std::move(converted);
        if (mono.empty()) {
            return true;
        }
    }

    // Kasus umum: satu blok cukup, kirim tanpa menyalin.
    if (mono.size() <= 5760) {
        if (!audio.IsRadioPlaybackActive()) {
            return false;
        }
        if (!audio.PushRadioPcm(std::move(mono), pdMS_TO_TICKS(500))) {
            ESP_LOGE(TAG, "Cannot push PCM to AudioService");
            return false;
        }
        return true;
    }

    // AudioService tetap satu-satunya penulis I2S.
    size_t offset = 0;

    while (offset < mono.size()) {
        if (!audio.IsRadioPlaybackActive()) {
            return false;
        }

        const size_t count = std::min<size_t>(5760, mono.size() - offset);

        std::vector<int16_t> block(
            mono.begin() + offset,
            mono.begin() + offset + count
        );

        if (!audio.PushRadioPcm(std::move(block), pdMS_TO_TICKS(500))) {
            ESP_LOGE(TAG, "Cannot push PCM to AudioService");
            return false;
        }

        offset += count;
    }

    return true;
}

// Dijalankan dari task terpisah setelah task radio dihapus, supaya stack task
// radio sudah bebas dan encoder Opus bisa mendapat blok heap yang utuh.
void RestoreAfterRadio() {
    auto& app = Application::GetInstance();
    auto& audio = app.GetAudioService();

    ESP_LOGI(TAG, "Restoring audio after radio (free heap %u, largest block %u)",
             static_cast<unsigned>(esp_get_free_heap_size()),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));

    audio.StopRadioPlayback(true);

    if (app.GetDeviceState() == kDeviceStateIdle) {
        // Pastikan voice processing tidak tertinggal aktif.
        audio.EnableVoiceProcessing(false);

        // Paksa aktifkan kembali pendeteksi Hi Jason.
        audio.EnableWakeWordDetection(true);

        // Kembali ke mode hemat daya.
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

        Board::GetInstance().GetDisplay()->SetStatus(Lang::Strings::STANDBY);

        ESP_LOGI(TAG, "Radio stopped: standby restored, wake word enabled");
    }

    ESP_LOGI(TAG, "Radio playback stopped");
}

}  // namespace

RadioPlayer::RadioPlayer() = default;

RadioPlayer::~RadioPlayer() {
    RequestStop();
}

bool RadioPlayer::StartRadio() {
    if (active_.load()) {
        if (startup_state_.load() == 1) {
            ESP_LOGI(TAG, "Radio is already playing");
            return true;
        }

        if (startup_state_.load() == 0) {
            ESP_LOGW(TAG, "Radio startup is still in progress");
        }

        return false;
    }

    auto& audio = Application::GetInstance().GetAudioService();

    if (!audio.StartRadioPlayback()) {
        ESP_LOGE(TAG, "AudioService refused radio playback");
        return false;
    }

    stop_requested_.store(false);
    startup_state_.store(0);
    active_.store(true);
    Board::GetInstance().GetDisplay()->SetStatus("RADIO");
    if (xTaskCreate(
            TaskEntry,
            "radio_player",
            8192,
            this,
            5,
            &task_handle_) != pdPASS) {
        task_handle_ = nullptr;
        active_.store(false);
        startup_state_.store(-1);
        audio.StopRadioPlayback(true);
        ESP_LOGE(TAG, "Cannot create radio task");
        return false;
    }

    int elapsed_ms = 0;

    while (elapsed_ms < kStartupTimeoutMs) {
        const int state = startup_state_.load();

        if (state == 1) {
            ESP_LOGI(TAG, "Radio startup confirmed");
            return true;
        }

        if (state == -1) {
            ESP_LOGE(TAG, "Radio startup failed");
            return false;
        }

        if (!active_.load()) {
            ESP_LOGE(TAG, "Radio task stopped during startup");
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
        elapsed_ms += 100;
    }

    ESP_LOGE(TAG, "Radio startup timeout");
    stop_requested_.store(true);
    return false;
}

void RadioPlayer::RequestStop() {
    stop_requested_.store(true);
}

void RadioPlayer::TaskEntry(void* arg) {
    auto* self = static_cast<RadioPlayer*>(arg);
    self->TaskLoop();

    // Tandai selesai supaya StartRadio() tidak mengira radio masih berjalan.
    // active_ tetap true sampai pemulihan audio selesai.
    self->startup_state_.store(-1);
    self->task_handle_ = nullptr;

    // Pemulihan dijalankan dari task lain setelah task ini dihapus,
    // supaya stack 8 KB task radio ikut bebas dan blok heap utuh lagi.
    const BaseType_t created = xTaskCreate(
        [](void* param) {
            auto* player = static_cast<RadioPlayer*>(param);
            // Beri waktu idle task membebaskan stack task radio.
            vTaskDelay(pdMS_TO_TICKS(500));
            RestoreAfterRadio();
            player->active_.store(false);
            vTaskDelete(nullptr);
        },
        "radio_restore", 4096, self, 5, nullptr);

    if (created != pdPASS) {
        ESP_LOGE(TAG, "Cannot create restore task, restoring inline");
        RestoreAfterRadio();
        self->active_.store(false);
    }

    vTaskDelete(nullptr);
}

void RadioPlayer::TaskLoop() {
    auto& audio = Application::GetInstance().GetAudioService();

    HttpResponseHeaders response_headers;

    // Matikan hemat daya WiFi selama radio supaya paket tidak tertunda.
    esp_wifi_set_ps(WIFI_PS_NONE);

    esp_http_client_handle_t client = nullptr;
    esp_audio_simple_dec_handle_t decoder = nullptr;
    LinearResampler resampler;

    bool decoder_registered = false;
    bool simple_decoder_registered = false;

    uint8_t* input_buffer = static_cast<uint8_t*>(malloc(kHttpReadSize));
    uint8_t* output_buffer = static_cast<uint8_t*>(malloc(kDecoderOutputSize));

    do {
        if (input_buffer == nullptr || output_buffer == nullptr) {
            ESP_LOGE(TAG, "Not enough memory to start radio");
            break;
        }

        // Coba stasiun satu per satu sampai ada yang tersambung.
        for (size_t i = 0; i < kStationCount; i++) {
            if (stop_requested_.load()) {
                break;
            }

            ESP_LOGI(TAG, "Trying station %u/%u: %s (free heap %u, largest block %u)",
                     static_cast<unsigned>(i + 1),
                     static_cast<unsigned>(kStationCount),
                     kStations[i].name,
                     static_cast<unsigned>(esp_get_free_heap_size()),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));

            client = ConnectStation(kStations[i], response_headers, stop_requested_);
            if (client != nullptr) {
                ESP_LOGI(TAG, "Connected to %s", kStations[i].name);
                break;
            }

            ESP_LOGW(TAG, "Station %s failed, trying next", kStations[i].name);
        }

        if (stop_requested_.load()) {
            break;
        }

        if (client == nullptr) {
            ESP_LOGE(TAG, "Cannot connect to any radio station");
            break;
        }

        // Registrasi decoder MP3.
        const auto register_err = esp_audio_dec_register_default();
        if (register_err != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "Cannot register audio decoders: %d", register_err);
            break;
        }
        decoder_registered = true;

        const auto simple_register_err = esp_audio_simple_dec_register_default();
        if (simple_register_err != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "Cannot register simple decoders: %d", simple_register_err);
            break;
        }
        simple_decoder_registered = true;

        esp_audio_simple_dec_cfg_t dec_cfg = {
            .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
            .dec_cfg = nullptr,
            .cfg_size = 0,
            .use_frame_dec = false,
        };

        const auto decoder_open_err = esp_audio_simple_dec_open(&dec_cfg, &decoder);
        if (decoder_open_err != ESP_AUDIO_ERR_OK || decoder == nullptr) {
            ESP_LOGE(TAG, "Cannot open MP3 decoder: %d", decoder_open_err);
            break;
        }

        ESP_LOGI(TAG, "MP3 decoder opened successfully (free heap %u, largest block %u)",
                 static_cast<unsigned>(esp_get_free_heap_size()),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
        bool logged_first_bytes = false;

        // Statistik 5 detik untuk mendiagnosis suara putus-putus.
        int64_t stat_window_start_us = esp_timer_get_time();
        int64_t stat_audio_us = 0;
        int64_t stat_dec_us = 0;
        int64_t stat_max_read_us = 0;
        int64_t stat_bytes = 0;
        unsigned stat_rate = 0;
        unsigned stat_ch = 0;

        // Loop streaming.
        while (!stop_requested_.load() && audio.IsRadioPlaybackActive()) {
            const int64_t read_t0 = esp_timer_get_time();
            const int read = esp_http_client_read(
                client,
                reinterpret_cast<char*>(input_buffer),
                kHttpReadSize
            );
            const int64_t read_us = esp_timer_get_time() - read_t0;
            if (read_us > stat_max_read_us) {
                stat_max_read_us = read_us;
            }

            if (esp_timer_get_time() - stat_window_start_us >= 5000000) {
                wifi_ap_record_t ap = {};
                const int rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
                ESP_LOGI(TAG, "Radio stats 5s: audio=%lld ms, decode=%lld ms, max_read=%lld ms, bytes=%lld, rssi=%d, %u Hz %u ch, free heap %u",
                         static_cast<long long>(stat_audio_us / 1000),
                         static_cast<long long>(stat_dec_us / 1000),
                         static_cast<long long>(stat_max_read_us / 1000),
                         static_cast<long long>(stat_bytes),
                         rssi, stat_rate, stat_ch,
                         static_cast<unsigned>(esp_get_free_heap_size()));
                stat_window_start_us = esp_timer_get_time();
                stat_audio_us = 0;
                stat_dec_us = 0;
                stat_max_read_us = 0;
                stat_bytes = 0;
            }

            if (read < 0) {
                ESP_LOGE(TAG, "Radio stream read failed: %d", read);
                break;
            }

            if (read == 0) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }

            stat_bytes += read;

            if (!logged_first_bytes) {
                logged_first_bytes = true;
                ESP_LOGI(TAG, "First stream bytes (%d read): %02X %02X %02X %02X %02X %02X %02X %02X",
                         read,
                         input_buffer[0], read > 1 ? input_buffer[1] : 0,
                         read > 2 ? input_buffer[2] : 0, read > 3 ? input_buffer[3] : 0,
                         read > 4 ? input_buffer[4] : 0, read > 5 ? input_buffer[5] : 0,
                         read > 6 ? input_buffer[6] : 0, read > 7 ? input_buffer[7] : 0);
            }

            esp_audio_simple_dec_raw_t raw = {
                .buffer = input_buffer,
                .len = static_cast<uint32_t>(read),
                .eos = false,
                .consumed = 0,
            };

            while (raw.len > 0 &&
                   !stop_requested_.load() &&
                   audio.IsRadioPlaybackActive()) {

                esp_audio_simple_dec_out_t frame = {
                    .buffer = output_buffer,
                    .len = kDecoderOutputSize,
                    .needed_size = 0,
                    .decoded_size = 0,
                };

                const int64_t dec_t0 = esp_timer_get_time();
                const auto dec_ret = esp_audio_simple_dec_process(decoder, &raw, &frame);
                stat_dec_us += esp_timer_get_time() - dec_t0;

                if (dec_ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH &&
                    frame.needed_size > kDecoderOutputSize) {
                    uint8_t* bigger = static_cast<uint8_t*>(
                        realloc(output_buffer, frame.needed_size));

                    if (bigger == nullptr) {
                        ESP_LOGE(TAG, "Cannot allocate decoder output buffer");
                        stop_requested_.store(true);
                        break;
                    }

                    output_buffer = bigger;
                    continue;
                }

                if (dec_ret != ESP_AUDIO_ERR_OK) {
                    ESP_LOGE(TAG, "MP3 decode error: %d (free heap %u, min %u, largest block %u)",
                             dec_ret,
                             static_cast<unsigned>(esp_get_free_heap_size()),
                             static_cast<unsigned>(esp_get_minimum_free_heap_size()),
                             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
                    stop_requested_.store(true);
                    break;
                }

                if (frame.decoded_size > 0) {
                    esp_audio_simple_dec_info_t info = {};

                    if (esp_audio_simple_dec_get_info(decoder, &info) != ESP_AUDIO_ERR_OK) {
                        ESP_LOGE(TAG, "Cannot get MP3 decoder info");
                        stop_requested_.store(true);
                        break;
                    }

                    if (info.sample_rate > 0 && info.channel > 0) {
                        stat_audio_us += static_cast<int64_t>(frame.decoded_size) * 1000000LL /
                                         (static_cast<int64_t>(info.sample_rate) * info.channel * 2);
                        stat_rate = info.sample_rate;
                        stat_ch = info.channel;
                    }

                    if (!ConvertAndQueue(audio, frame.buffer, frame.decoded_size,
                                         info, resampler)) {
                        ESP_LOGE(TAG, "Could not queue radio PCM");
                        stop_requested_.store(true);
                        break;
                    }

                    if (startup_state_.load() == 0) {
                        startup_state_.store(1);
                        ESP_LOGI(TAG, "First radio PCM queued successfully");
                    }
                }

                if (raw.consumed == 0) {
                    break;
                }

                raw.len -= raw.consumed;
                raw.buffer += raw.consumed;
                raw.consumed = 0;
            }
        }
    } while (false);

    // Bersihkan resource stream.
    if (client != nullptr) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }

    if (decoder != nullptr) {
        esp_audio_simple_dec_close(decoder);
    }

    if (simple_decoder_registered) {
        esp_audio_simple_dec_unregister_default();
    }

    if (decoder_registered) {
        esp_audio_dec_unregister_default();
    }

    free(input_buffer);
    free(output_buffer);

    if (startup_state_.load() == 0) {
        startup_state_.store(-1);
    }

    // Pemulihan Opus/wake word TIDAK dilakukan di sini. Task radio masih
    // memegang stack 8 KB; pemulihan dijalankan di TaskEntry lewat task
    // "radio_restore" setelah task ini dihapus.
    ESP_LOGI(TAG, "Radio stream task finished (free heap %u, largest block %u)",
             static_cast<unsigned>(esp_get_free_heap_size()),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}
