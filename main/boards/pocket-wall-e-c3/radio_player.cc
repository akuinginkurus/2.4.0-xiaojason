
#include "radio_player.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <strings.h>
#include <vector>
#include <utility>

#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_crt_bundle.h>
#include <esp_audio_simple_dec.h>
#include <esp_audio_simple_dec_default.h>
#include <esp_ae_rate_cvt.h>

#include "application.h"
#include "audio/audio_service.h"

#define TAG "RadioPlayer"

namespace {

// Radio Indonesia: Elshinta FM
// MP3 64 kbps
constexpr char kRadioUrl[] =
    "https://stream-ssl.arenastreaming.com:8000/jakarta";

constexpr size_t kHttpReadSize = 1024;
constexpr size_t kDecoderOutputSize = 8192;

constexpr int kRadioOutputRate = 24000;
constexpr int kMaxRedirects = 5;
constexpr int kStartupTimeoutMs = 20000;

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

        auto* headers =
            static_cast<HttpResponseHeaders*>(event->user_data);

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

bool ConvertAndQueue(
    AudioService& audio,
    const uint8_t* pcm_bytes,
    size_t byte_count,
    const esp_audio_simple_dec_info_t& info,
    esp_ae_rate_cvt_handle_t& resampler) {

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

    const auto* input =
        reinterpret_cast<const int16_t*>(pcm_bytes);

    const size_t input_samples =
        byte_count / sizeof(int16_t);

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

    // Resampling tetap 24 kHz.
    if (info.sample_rate != kRadioOutputRate) {

        if (resampler == nullptr) {
            esp_ae_rate_cvt_cfg_t cfg = {
                .src_rate = info.sample_rate,
                .dest_rate = kRadioOutputRate,
                .channel = 1,
                .bits_per_sample = ESP_AUDIO_BIT16,
                .complexity = 2,
                .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
            };

            const auto err =
                esp_ae_rate_cvt_open(&cfg, &resampler);

            if (err != ESP_AE_ERR_OK || resampler == nullptr) {
                ESP_LOGE(TAG, "Cannot create resampler: %d", err);
                return false;
            }
        }

        uint32_t max_output = 0;

        if (esp_ae_rate_cvt_get_max_out_sample_num(
                resampler,
                mono.size(),
                &max_output) != ESP_AE_ERR_OK) {

            ESP_LOGE(TAG, "Cannot get resampler output size");
            return false;
        }

        std::vector<int16_t> converted(max_output);
        uint32_t actual_output = max_output;

        if (esp_ae_rate_cvt_process(
                resampler,
                reinterpret_cast<esp_ae_sample_t>(mono.data()),
                mono.size(),
                reinterpret_cast<esp_ae_sample_t>(converted.data()),
                &actual_output) != ESP_AE_ERR_OK) {

            ESP_LOGE(TAG, "Resampling failed");
            return false;
        }

        converted.resize(actual_output);
        mono = std::move(converted);
    }

    // AudioService tetap satu-satunya penulis I2S.
    size_t offset = 0;

    while (offset < mono.size()) {

        if (!audio.IsRadioPlaybackActive()) {
            return false;
        }

        const size_t count =
            std::min<size_t>(5760, mono.size() - offset);

        std::vector<int16_t> block(
            mono.begin() + offset,
            mono.begin() + offset + count
        );

        if (!audio.PushRadioPcm(
                std::move(block),
                pdMS_TO_TICKS(500))) {

            ESP_LOGE(TAG, "Cannot push PCM to AudioService");
            return false;
        }

        offset += count;
    }

    return true;
}

} // namespace


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

    auto& audio =
        Application::GetInstance().GetAudioService();

    if (!audio.StartRadioPlayback()) {
        ESP_LOGE(TAG, "AudioService refused radio playback");
        return false;
    }

    stop_requested_.store(false);
    startup_state_.store(0);
    active_.store(true);

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

    if (self->startup_state_.load() == 0) {
        self->startup_state_.store(-1);
    }

    self->task_handle_ = nullptr;
    self->active_.store(false);

    vTaskDelete(nullptr);
}


void RadioPlayer::TaskLoop() {

    auto& audio =
        Application::GetInstance().GetAudioService();

    HttpResponseHeaders response_headers;

    esp_http_client_config_t http_cfg = {};

    http_cfg.url = kRadioUrl;
    http_cfg.timeout_ms = 10000;
    http_cfg.buffer_size = 2048;
    http_cfg.buffer_size_tx = 512;

    // Redirect ditangani manual.
    http_cfg.disable_auto_redirect = true;
    http_cfg.max_redirection_count = kMaxRedirects;

    http_cfg.event_handler = HttpEventHandler;
    http_cfg.user_data = &response_headers;

    // HTTPS certificate verification.
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client =
        esp_http_client_init(&http_cfg);

    esp_audio_simple_dec_handle_t decoder = nullptr;
    esp_ae_rate_cvt_handle_t resampler = nullptr;

    bool decoder_registered = false;
    bool simple_decoder_registered = false;

    uint8_t* input_buffer =
        static_cast<uint8_t*>(malloc(kHttpReadSize));

    uint8_t* output_buffer =
        static_cast<uint8_t*>(malloc(kDecoderOutputSize));

    do {

        if (client == nullptr ||
            input_buffer == nullptr ||
            output_buffer == nullptr) {

            ESP_LOGE(TAG, "Not enough memory to start radio");
            break;
        }

        bool connected = false;
        int status = -1;
        int64_t content_length = -1;

        // Buka koneksi dan ikuti redirect.
        for (int redirect = 0;
             redirect <= kMaxRedirects;
             redirect++) {

            if (stop_requested_.load()) {
                break;
            }

            response_headers.content_type.clear();

            const esp_err_t open_err =
                esp_http_client_open(client, 0);

            if (open_err != ESP_OK) {
                ESP_LOGE(
                    TAG,
                    "HTTP open failed: %s",
                    esp_err_to_name(open_err)
                );
                break;
            }

            content_length =
                esp_http_client_fetch_headers(client);

            status =
                esp_http_client_get_status_code(client);

            ESP_LOGI(TAG, "HTTP status: %d", status);

            if (status == 301 ||
                status == 302 ||
                status == 303 ||
                status == 307 ||
                status == 308) {

                ESP_LOGW(
                    TAG,
                    "Radio redirect %d/%d",
                    redirect + 1,
                    kMaxRedirects
                );

                if (redirect >= kMaxRedirects) {
                    ESP_LOGE(TAG, "Too many radio redirects");
                    esp_http_client_close(client);
                    break;
                }

                const esp_err_t redirect_err =
                    esp_http_client_set_redirection(client);

                esp_http_client_close(client);

                if (redirect_err != ESP_OK) {
                    ESP_LOGE(
                        TAG,
                        "Cannot follow redirect: %s",
                        esp_err_to_name(redirect_err)
                    );
                    break;
                }

                continue;
            }

            if (status == 200 || status == 206) {
                connected = true;
            } else {
                ESP_LOGE(TAG, "Radio HTTP status error: %d", status);
            }

            break;
        }

        if (stop_requested_.load()) {
            break;
        }

        if (!connected) {
            ESP_LOGE(TAG, "Cannot connect to radio stream");
            break;
        }

        ESP_LOGI(
            TAG,
            "Radio Content-Type: %s",
            response_headers.content_type.empty()
                ? "(empty)"
                : response_headers.content_type.c_str()
        );

        if (!IsMp3ContentType(response_headers.content_type)) {
            ESP_LOGE(
                TAG,
                "Unsupported radio Content-Type: %s",
                response_headers.content_type.c_str()
            );
            break;
        }

        ESP_LOGI(
            TAG,
            "Connected to Indonesian MP3 radio, length=%lld",
            static_cast<long long>(content_length)
        );

        // Registrasi decoder MP3.
        const auto register_err =
            esp_audio_dec_register_default();

        if (register_err != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(
                TAG,
                "Cannot register audio decoders: %d",
                register_err
            );
            break;
        }

        decoder_registered = true;

        const auto simple_register_err =
            esp_audio_simple_dec_register_default();

        if (simple_register_err != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(
                TAG,
                "Cannot register simple decoders: %d",
                simple_register_err
            );
            break;
        }

        simple_decoder_registered = true;

        esp_audio_simple_dec_cfg_t dec_cfg = {
            .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
            .dec_cfg = nullptr,
            .cfg_size = 0,
            .use_frame_dec = false,
        };

        const auto decoder_open_err =
            esp_audio_simple_dec_open(&dec_cfg, &decoder);

        if (decoder_open_err != ESP_AUDIO_ERR_OK ||
            decoder == nullptr) {

            ESP_LOGE(
                TAG,
                "Cannot open MP3 decoder: %d",
                decoder_open_err
            );
            break;
        }

        ESP_LOGI(TAG, "MP3 decoder opened successfully");

        // Loop streaming.
        while (!stop_requested_.load() &&
               audio.IsRadioPlaybackActive()) {

            const int read =
                esp_http_client_read(
                    client,
                    reinterpret_cast<char*>(input_buffer),
                    kHttpReadSize
                );

            if (read < 0) {
                ESP_LOGE(TAG, "Radio stream read failed: %d", read);
                break;
            }

            if (read == 0) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }

            ESP_LOGD(TAG, "Received %d MP3 bytes", read);

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

                const auto dec_ret =
                    esp_audio_simple_dec_process(
                        decoder,
                        &raw,
                        &frame
                    );

                if (dec_ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH &&
                    frame.needed_size > kDecoderOutputSize) {

                    uint8_t* bigger =
                        static_cast<uint8_t*>(
                            realloc(
                                output_buffer,
                                frame.needed_size
                            )
                        );

                    if (bigger == nullptr) {
                        ESP_LOGE(
                            TAG,
                            "Cannot allocate decoder output buffer"
                        );

                        stop_requested_.store(true);
                        break;
                    }

                    output_buffer = bigger;
                    continue;
                }

                if (dec_ret != ESP_AUDIO_ERR_OK) {
                    ESP_LOGE(
                        TAG,
                        "MP3 decode error: %d",
                        dec_ret
                    );

                    stop_requested_.store(true);
                    break;
                }

                if (frame.decoded_size > 0) {

                    esp_audio_simple_dec_info_t info = {};

                    if (esp_audio_simple_dec_get_info(
                            decoder,
                            &info) != ESP_AUDIO_ERR_OK) {

                        ESP_LOGE(TAG, "Cannot get MP3 decoder info");
                        stop_requested_.store(true);
                        break;
                    }

                    ESP_LOGD(
                        TAG,
                        "MP3 decoded: %lu bytes, %u Hz, %u ch, %u bit",
                        static_cast<unsigned long>(frame.decoded_size),
                        static_cast<unsigned>(info.sample_rate),
                        static_cast<unsigned>(info.channel),
                        static_cast<unsigned>(info.bits_per_sample)
                    );

                    if (!ConvertAndQueue(
                            audio,
                            frame.buffer,
                            frame.decoded_size,
                            info,
                            resampler)) {

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


    // Bersihkan resource.
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

    if (resampler != nullptr) {
        esp_ae_rate_cvt_close(resampler);
    }

    free(input_buffer);
    free(output_buffer);

    if (startup_state_.load() == 0) {
        startup_state_.store(-1);
    }

    audio.StopRadioPlayback(true);

    ESP_LOGI(TAG, "Radio playback stopped");
}
