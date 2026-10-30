
#include "radio_player.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <strings.h>
#include <vector>
#include <utility>

#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_audio_simple_dec.h>
#include <esp_audio_simple_dec_default.h>
#include <esp_aac_dec.h>
#include <esp_ae_rate_cvt.h>

#include "application.h"
#include "audio/audio_service.h"

#define TAG "PramborsRadio"

namespace {

// Prambors MP3 stream.
// HTTP digunakan untuk menghindari masalah sertifikat TLS pada ESP32-C3.
constexpr char kPramborsUrl[] =
    "http://22253.live.streamtheworld.com/PRAMBORS_FM.mp3";

constexpr size_t kHttpReadSize = 1024;
constexpr size_t kDecoderOutputSize = 8192;
constexpr int kRadioOutputRate = 24000;
constexpr int kMaxRedirects = 5;

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

struct DecoderConfig {
    esp_aac_dec_cfg_t aac;
};

esp_audio_simple_dec_type_t SelectDecoder(
    const char* content_type) {

    if (content_type == nullptr) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    }

    if (strstr(content_type, "mpeg") != nullptr ||
        strstr(content_type, "mp3") != nullptr) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    }

    // Radio ini ditujukan untuk MP3.
    // AAC tidak digunakan.
    return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
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

    // Resampling ke 24 kHz.
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
                ESP_LOGE(
                    TAG,
                    "Cannot create resampler: %d",
                    err
                );
                return false;
            }
        }

        uint32_t max_output = 0;

        if (esp_ae_rate_cvt_get_max_out_sample_num(
                resampler,
                mono.size(),
                &max_output) != ESP_AE_ERR_OK) {
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
            return false;
        }

        converted.resize(actual_output);
        mono = std::move(converted);
    }

    // AudioOutputTask tetap menjadi satu-satunya task
    // yang menulis data ke I2S.
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


bool RadioPlayer::StartPrambors() {

    if (active_.load()) {
        return true;
    }

    auto& audio =
        Application::GetInstance().GetAudioService();

    if (!audio.StartRadioPlayback()) {
        ESP_LOGE(TAG, "AudioService refused radio playback");
        return false;
    }

    stop_requested_.store(false);
    active_.store(true);

    if (xTaskCreate(
            TaskEntry,
            "prambors_radio",
            8192,
            this,
            5,
            &task_handle_) != pdPASS) {

        task_handle_ = nullptr;
        active_.store(false);

        audio.StopRadioPlayback(true);

        ESP_LOGE(TAG, "Cannot create radio task");
        return false;
    }

    return true;
}


void RadioPlayer::RequestStop() {
    stop_requested_.store(true);
}


void RadioPlayer::TaskEntry(void* arg) {

    auto* self = static_cast<RadioPlayer*>(arg);

    self->TaskLoop();

    self->task_handle_ = nullptr;
    self->active_.store(false);

    vTaskDelete(nullptr);
}


void RadioPlayer::TaskLoop() {

    auto& audio =
        Application::GetInstance().GetAudioService();

    HttpResponseHeaders response_headers;

    esp_http_client_config_t http_cfg = {};

    http_cfg.url = kPramborsUrl;
    http_cfg.timeout_ms = 5000;
    http_cfg.buffer_size = 2048;
    http_cfg.buffer_size_tx = 512;

    // Redirect ditangani manual.
    http_cfg.disable_auto_redirect = true;
    http_cfg.max_redirection_count = kMaxRedirects;

    http_cfg.event_handler = HttpEventHandler;
    http_cfg.user_data = &response_headers;

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

        // Buka koneksi dan ikuti redirect.
        bool connected = false;
        int status = -1;
        int64_t content_length = -1;

        for (int redirect = 0;
             redirect <= kMaxRedirects;
             redirect++) {

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
                ESP_LOGE(
                    TAG,
                    "Radio HTTP status error: %d",
                    status
                );
            }

            break;
        }

        if (!connected) {
            ESP_LOGE(TAG, "Cannot connect to Prambors MP3 stream");
            break;
        }

        if (response_headers.content_type.empty()) {
            ESP_LOGE(TAG, "Radio response has no Content-Type");
            break;
        }

        ESP_LOGI(
            TAG,
            "Radio Content-Type: %s",
            response_headers.content_type.c_str()
        );

        const auto decoder_type =
            SelectDecoder(response_headers.content_type.c_str());

        if (decoder_type != ESP_AUDIO_SIMPLE_DEC_TYPE_MP3) {
            ESP_LOGE(
                TAG,
                "Stream is not MP3. Content-Type: %s",
                response_headers.content_type.c_str()
            );
            break;
        }

        ESP_LOGI(
            TAG,
            "Connected to Prambors MP3, length=%lld",
            static_cast<long long>(content_length)
        );

        // Registrasi decoder.
        if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "Cannot register audio decoders");
            break;
        }

        decoder_registered = true;

        if (esp_audio_simple_dec_register_default() !=
            ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "Cannot register simple decoders");
            break;
        }

        simple_decoder_registered = true;

        // MP3 tidak membutuhkan konfigurasi AAC.
        esp_audio_simple_dec_cfg_t dec_cfg = {
            .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
            .dec_cfg = nullptr,
            .cfg_size = 0,
            .use_frame_dec = false,
        };

        if (esp_audio_simple_dec_open(
                &dec_cfg,
                &decoder) != ESP_AUDIO_ERR_OK ||
            decoder == nullptr) {

            ESP_LOGE(TAG, "Cannot open MP3 decoder");
            break;
        }

        ESP_LOGI(TAG, "Prambors MP3 decoder started");

        // Loop streaming radio.
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
                            &info) != ESP_AUDIO_ERR_OK ||
                        !ConvertAndQueue(
                            audio,
                            frame.buffer,
                            frame.decoded_size,
                            info,
                            resampler)) {

                        ESP_LOGE(TAG, "Could not queue radio PCM");

                        stop_requested_.store(true);
                        break;
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

    audio.StopRadioPlayback(true);

    ESP_LOGI(TAG, "Prambors playback stopped");
}
