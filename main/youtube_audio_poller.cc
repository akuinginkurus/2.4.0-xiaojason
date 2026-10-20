#include "youtube_audio_poller.h"

#include <nvs.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cstring>
#include <cctype>
#include <memory>

#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio/audio_service.h"
#include "assets/lang_config.h"

static const char* TAG = "YTPoller";

std::string YouTubeAudioPoller::GetServerDomain() {
    nvs_handle_t nvs;
    char buf[64] = {};
    size_t len = sizeof(buf);
    if (nvs_open("system", NVS_READONLY, &nvs) == ESP_OK) {
        if (nvs_get_str(nvs, "server_domain", buf, &len) == ESP_OK && len > 0) {
            nvs_close(nvs);
            return std::string(buf);
        }
        nvs_close(nvs);
    }
    return "xiaozhiscig.biz.id";
}

std::string YouTubeAudioPoller::GetBaseUrl() {
    std::string domain = GetServerDomain();
    if (domain.rfind("http://", 0) == 0 || domain.rfind("https://", 0) == 0) {
        return domain;
    }
    return "http://" + domain;
}

std::string YouTubeAudioPoller::GetStreamUrl() {
    return GetBaseUrl();
}

std::string YouTubeAudioPoller::GetCurrentTitle() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(song_mutex_));
    return current_title_;
}

std::string YouTubeAudioPoller::GetCurrentVideoId() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(song_mutex_));
    return current_video_id_;
}

YouTubeAudioPoller::YouTubeAudioPoller() {}

YouTubeAudioPoller& YouTubeAudioPoller::GetInstance() {
    static YouTubeAudioPoller instance;
    return instance;
}

void YouTubeAudioPoller::Start() {
    if (running_.load()) {
        return;
    }
    running_ = true;
    abort_requested_ = false;

    xTaskCreate(
        [](void* arg) {
            static_cast<YouTubeAudioPoller*>(arg)->TaskLoop();
            vTaskDelete(NULL);
        },
        "yt_poller", 3584, this, 2, &task_handle_);
    ESP_LOGI(TAG, "YouTubeAudioPoller started (server=%s)", GetServerDomain().c_str());
}

void YouTubeAudioPoller::Stop() {
    if (!running_.load()) {
        return;
    }
    running_ = false;
    abort_requested_ = true;
    task_handle_ = nullptr;
    ESP_LOGI(TAG, "YouTubeAudioPoller stopped");
}

void YouTubeAudioPoller::Abort() {
    if (is_playing_.load()) {
        ESP_LOGI(TAG, "Abort requested for YouTube audio stream");
        abort_requested_ = true;
        Application::GetInstance().GetAudioService().ResetDecoder();
        std::lock_guard<std::mutex> lock(http_mutex_);
        if (active_http_ != nullptr) {
            ESP_LOGI(TAG, "Force-closing active HTTP connection");
            active_http_->Close();
        }
    }
}

void YouTubeAudioPoller::TriggerFetch() {
    ESP_LOGI(TAG, "Fetch triggered externally");
    trigger_requested_ = true;
    Application::GetInstance().GetAudioService().EnableWakeWordDetection(false);
}

void YouTubeAudioPoller::PlaySong(const std::string& video_id, const std::string& title) {
    if (video_id.empty()) {
        return;
    }
    ESP_LOGI(TAG, "PlaySong: title=\"%s\", vid=%s", title.c_str(), video_id.c_str());
    {
        std::lock_guard<std::mutex> lock(song_mutex_);
        pending_video_id_ = video_id;
        pending_title_ = title.empty() ? "YouTube Song" : title;
    }
    play_requested_ = true;
    Application::GetInstance().GetAudioService().EnableWakeWordDetection(false);
}

void YouTubeAudioPoller::PlayQuery(const std::string& query) {
    if (query.empty()) {
        return;
    }
    if (is_playing_.load() || play_requested_.load()) {
        ESP_LOGW(TAG, "PlayQuery ignored (already playing): \"%s\"", query.c_str());
        return;
    }
    ESP_LOGI(TAG, "PlayQuery: \"%s\"", query.c_str());
    {
        std::lock_guard<std::mutex> lock(song_mutex_);
        pending_query_ = query;
    }
    query_requested_ = true;
    Application::GetInstance().GetAudioService().EnableWakeWordDetection(false);
}

static std::string ReadHttpBounded(Http* http, size_t max_bytes = 4096) {
    if (http == nullptr) {
        return "";
    }
    std::string body;
    char chunk[256];
    while (body.size() < max_bytes) {
        int n = http->Read(chunk, sizeof(chunk));
        if (n <= 0) {
            break;
        }
        body.append(chunk, n);
    }
    return body;
}

static void DrainHttp(Http* http) {
    if (http == nullptr) {
        return;
    }
    char drain[128];
    while (http->Read(drain, sizeof(drain)) > 0) {
    }
}

bool YouTubeAudioPoller::FetchDirect(const std::string& query, std::string& video_id,
                                     std::string& title) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return false;
    }

    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        return false;
    }

    std::string encoded_q;
    for (char c : query) {
        if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.') {
            encoded_q += c;
        } else if (c == ' ') {
            encoded_q += "%20";
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", static_cast<unsigned char>(c));
            encoded_q += hex;
        }
    }

    std::string mac = SystemInfo::GetMacAddress();
    std::string url = GetBaseUrl() + "/api/audio/play_direct?q=" + encoded_q + "&mac=" + mac;
    ESP_LOGI(TAG, "FetchDirect: %s", url.c_str());
    http->SetTimeout(12000);
    http->SetHeader("User-Agent", "ESP32-XiaoZhi");
    http->SetHeader("Device-Id", mac.c_str());
    http->SetHeader("X-Device-Mac", mac.c_str());

    if (!http->Open("GET", url)) {
        ESP_LOGW(TAG, "FetchDirect failed to open");
        http->Close();
        return false;
    }

    if (http->GetStatusCode() != 200) {
        ESP_LOGW(TAG, "FetchDirect HTTP %d", http->GetStatusCode());
        http->Close();
        return false;
    }

    std::string body = ReadHttpBounded(http.get(), 4096);
    http->Close();
    if (body.empty()) {
        return false;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return false;
    }

    bool found = false;
    cJSON* vid_item = cJSON_GetObjectItem(root, "video_id");
    cJSON* title_item = cJSON_GetObjectItem(root, "title");
    if (cJSON_IsString(vid_item)) {
        video_id = vid_item->valuestring;
        title = cJSON_IsString(title_item) ? title_item->valuestring : query;
        found = true;
    }
    cJSON_Delete(root);
    return found;
}

bool YouTubeAudioPoller::FetchCommands(int& cmd_id, std::string& video_id, std::string& title) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return false;
    }

    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        return false;
    }

    std::string mac = SystemInfo::GetMacAddress();
    std::string chip = CONFIG_IDF_TARGET;
    std::string url = GetBaseUrl() + "/api/audio/commands/" + mac;

    http->SetTimeout(10000);
    http->SetHeader("User-Agent", "ESP32-XiaoZhi");
    http->SetHeader("Device-Id", mac.c_str());
    http->SetHeader("X-Device-Mac", mac.c_str());
    http->SetHeader("X-Device-Chip", chip.c_str());
    http->SetHeader("Device-Chip", chip.c_str());

    if (!http->Open("GET", url)) {
        http->Close();
        return false;
    }
    if (http->GetStatusCode() != 200) {
        http->Close();
        return false;
    }

    std::string body = ReadHttpBounded(http.get(), 4096);
    http->Close();
    if (body.empty()) {
        return false;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return false;
    }

    bool found = false;
    cJSON* commands = cJSON_GetObjectItem(root, "commands");
    if (cJSON_IsArray(commands) && cJSON_GetArraySize(commands) > 0) {
        cJSON* cmd = cJSON_GetArrayItem(commands, 0);
        if (cmd != nullptr) {
            cJSON* id_item = cJSON_GetObjectItem(cmd, "id");
            cJSON* vid_item = cJSON_GetObjectItem(cmd, "video_id");
            cJSON* title_item = cJSON_GetObjectItem(cmd, "title");
            if ((cJSON_IsNumber(id_item) || cJSON_IsString(id_item)) && cJSON_IsString(vid_item)) {
                cmd_id = cJSON_IsNumber(id_item) ? id_item->valueint : atoi(id_item->valuestring);
                video_id = vid_item->valuestring;
                title = cJSON_IsString(title_item) ? title_item->valuestring : "YouTube Song";
                found = true;
            }
        }
    }
    cJSON_Delete(root);
    return found;
}

bool YouTubeAudioPoller::AckCommand(int cmd_id) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return false;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        return false;
    }

    std::string mac = SystemInfo::GetMacAddress();
    std::string chip = CONFIG_IDF_TARGET;
    std::string url = GetBaseUrl() + "/api/audio/ack/" + std::to_string(cmd_id);

    http->SetTimeout(10000);
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("User-Agent", "ESP32-XiaoZhi");
    http->SetHeader("Device-Id", mac.c_str());
    http->SetHeader("X-Device-Mac", mac.c_str());
    http->SetHeader("X-Device-Chip", chip.c_str());
    http->SetHeader("Device-Chip", chip.c_str());

    bool ok = http->Open("POST", url);
    if (ok) {
        DrainHttp(http.get());
    }
    http->Close();
    return ok;
}

bool YouTubeAudioPoller::ReportStatus(const std::string& status, const std::string& video_id) {
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 40000) {
        return false;
    }

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return false;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        return false;
    }

    std::string mac = SystemInfo::GetMacAddress();
    std::string chip = CONFIG_IDF_TARGET;
    std::string url = GetBaseUrl() + "/api/device/audio/status";

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", status.c_str());
    cJSON_AddStringToObject(root, "mac", mac.c_str());
    cJSON_AddStringToObject(root, "chip", chip.c_str());
    if (!video_id.empty()) {
        cJSON_AddStringToObject(root, "video_id", video_id.c_str());
    }
    char* json_str = cJSON_PrintUnformatted(root);
    std::string payload = json_str ? json_str : "";
    if (json_str) {
        free(json_str);
    }
    cJSON_Delete(root);

    http->SetTimeout(6000);
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("User-Agent", "ESP32-XiaoZhi");
    http->SetHeader("Device-Id", mac.c_str());
    http->SetHeader("X-Device-Mac", mac.c_str());
    http->SetHeader("X-Device-Chip", chip.c_str());
    http->SetHeader("Device-Chip", chip.c_str());
    http->SetContent(std::move(payload));

    bool ok = http->Open("POST", url);
    if (ok) {
        DrainHttp(http.get());
    }
    http->Close();
    return ok;
}

void YouTubeAudioPoller::StreamAudio(const std::string& video_id, const std::string& title) {
    auto& app = Application::GetInstance();
    auto display = Board::GetInstance().GetDisplay();

    {
        std::lock_guard<std::mutex> lock(song_mutex_);
        current_video_id_ = video_id;
        current_title_ = title;
    }
    is_playing_ = true;
    abort_requested_ = false;

    ESP_LOGI(TAG, "Start YouTube: \"%s\" (vid=%s)", title.c_str(), video_id.c_str());
    app.GetAudioService().EnableWakeWordDetection(false);

    {
        int wait_ms = 0;
        while (app.GetDeviceState() == kDeviceStateSpeaking && wait_ms < 8000 &&
               !abort_requested_.load()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            wait_ms += 100;
        }
    }

    if (abort_requested_.load()) {
        is_playing_ = false;
        {
            std::lock_guard<std::mutex> lock(song_mutex_);
            current_video_id_.clear();
            current_title_.clear();
        }
        app.SetDeviceState(kDeviceStateIdle);
        return;
    }

    app.SetDeviceState(kDeviceStateIdle);
    play_requested_ = false;
    query_requested_ = false;
    ReportStatus("playing", video_id);

    std::string display_title = "[PLAY] " + title;
    if (display != nullptr) {
        display->SetChatMessage("assistant", display_title.c_str());
        display->SetStatus("Memutar Lagu");
        display->SetEmotion("happy");
        display->ShowNotification(display_title.c_str(), 5000);
    }

    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    esp_wifi_set_ps(WIFI_PS_NONE);
    vTaskDelay(pdMS_TO_TICKS(30));

    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec != nullptr && !codec->output_enabled()) {
        codec->EnableOutput(true);
    }

    app.GetAudioService().ResetDecoder();
    vTaskDelay(pdMS_TO_TICKS(50));

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        is_playing_ = false;
        {
            std::lock_guard<std::mutex> lock(song_mutex_);
            current_video_id_.clear();
            current_title_.clear();
        }
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        app.SetDeviceState(kDeviceStateIdle);
        return;
    }

    wifi_ap_record_t ap_info;
    int8_t rssi = -100;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    }
    size_t free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    std::string current_bitrate = "12k";
    if (rssi >= -68 && free_sram >= 36000) {
        current_bitrate = "16k";
    } else if (rssi >= -78 && free_sram >= 28000) {
        current_bitrate = "12k";
    } else {
        current_bitrate = "8k";
    }
    ESP_LOGI(TAG, "Adaptive bitrate: RSSI=%d free=%u -> %s", rssi, (unsigned)free_sram,
             current_bitrate.c_str());

    static const uint8_t OGGS_MAGIC[4] = {'O', 'g', 'g', 'S'};
    static const size_t kOggBufCap = 2048;
    auto ogg_buf = std::make_unique<uint8_t[]>(kOggBufCap);

    size_t total_bytes_streamed = 0;
    int frames_pushed = 0;
    int downshift_retries = 0;
    int eof_retries = 0;
    static const int kMaxRetries = 3;
    int64_t last_rssi_check_us = 0;
    int8_t live_rssi = rssi;

    while (running_.load() && !abort_requested_.load()) {
        auto http = network->CreateHttp(0);
        if (http == nullptr) {
            break;
        }
        {
            std::lock_guard<std::mutex> lock(http_mutex_);
            active_http_ = http.get();
        }

        float start_sec = frames_pushed * 0.060f;
        char url_buf[320];
        std::string mac = SystemInfo::GetMacAddress();
        std::string chip = CONFIG_IDF_TARGET;
        if (start_sec > 0.5f) {
            snprintf(url_buf, sizeof(url_buf),
                     "%s/api/audio/stream/%s?br=%s&start=%.2f&mac=%s&chip=%s",
                     GetStreamUrl().c_str(), video_id.c_str(), current_bitrate.c_str(), start_sec,
                     mac.c_str(), chip.c_str());
        } else {
            snprintf(url_buf, sizeof(url_buf), "%s/api/audio/stream/%s?br=%s&mac=%s&chip=%s",
                     GetStreamUrl().c_str(), video_id.c_str(), current_bitrate.c_str(), mac.c_str(),
                     chip.c_str());
        }
        std::string stream_url(url_buf);

        http->SetTimeout(15000);
        http->SetHeader("User-Agent", "ESP32-XiaoZhi");
        http->SetHeader("Device-Id", mac.c_str());
        http->SetHeader("X-Device-Mac", mac.c_str());
        http->SetHeader("X-Device-Chip", chip.c_str());
        http->SetHeader("Device-Chip", chip.c_str());

        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        esp_wifi_set_ps(WIFI_PS_NONE);
        ESP_LOGI(TAG, "Stream connect: %s", stream_url.c_str());

        if (!http->Open("GET", stream_url)) {
            {
                std::lock_guard<std::mutex> lock(http_mutex_);
                active_http_ = nullptr;
            }
            http->Close();
            if (frames_pushed > 0 && downshift_retries < kMaxRetries && !abort_requested_.load()) {
                downshift_retries++;
                vTaskDelay(pdMS_TO_TICKS(500 * downshift_retries));
                continue;
            }
            break;
        }

        if (http->GetStatusCode() != 200) {
            ESP_LOGE(TAG, "Stream HTTP %d", http->GetStatusCode());
            {
                std::lock_guard<std::mutex> lock(http_mutex_);
                active_http_ = nullptr;
            }
            http->Close();
            if (frames_pushed > 0 && downshift_retries < kMaxRetries && !abort_requested_.load()) {
                downshift_retries++;
                vTaskDelay(pdMS_TO_TICKS(500 * downshift_retries));
                continue;
            }
            break;
        }

        downshift_retries = 0;
        size_t ogg_len = 0;
        bool seen_head = false;
        bool seen_tags = false;
        bool need_downshift = false;
        int64_t last_audio_active_us = esp_timer_get_time();
        size_t session_bytes = 0;
        int session_frames = 0;

        size_t prebuffer_target = (live_rssi >= -75 && frames_pushed == 0) ? 4 : 6;
        std::vector<std::unique_ptr<AudioStreamPacket>> prebuffer;
        bool is_prebuffering = true;

        while (running_.load() && !abort_requested_.load()) {
            int64_t now_us = esp_timer_get_time();
            bool playback_idle = app.GetAudioService().IsPlaybackIdle();

            if (now_us - last_rssi_check_us > 5000000) {
                last_rssi_check_us = now_us;
                wifi_ap_record_t live_ap;
                if (esp_wifi_sta_get_ap_info(&live_ap) == ESP_OK) {
                    live_rssi = live_ap.rssi;
                }
            }

            if (frames_pushed > 10) {
                if (!playback_idle) {
                    last_audio_active_us = now_us;
                } else if (now_us - last_audio_active_us > 2500000) {
                    ESP_LOGW(TAG, "Audio starvation, downshift from %s", current_bitrate.c_str());
                    if (current_bitrate == "16k")
                        current_bitrate = "12k";
                    else if (current_bitrate == "12k")
                        current_bitrate = "8k";
                    need_downshift = true;
                    if (current_bitrate == "8k") {
                        downshift_retries++;
                    }
                    break;
                }
            }

            size_t avail = kOggBufCap - ogg_len;
            if (avail == 0) {
                size_t next_oggs = 1;
                while (next_oggs + 4 <= ogg_len &&
                       memcmp(ogg_buf.get() + next_oggs, OGGS_MAGIC, 4) != 0) {
                    next_oggs++;
                }
                if (next_oggs < ogg_len) {
                    memmove(ogg_buf.get(), ogg_buf.get() + next_oggs, ogg_len - next_oggs);
                    ogg_len -= next_oggs;
                } else {
                    ogg_len = 0;
                }
                avail = kOggBufCap - ogg_len;
            }

            int to_read = (int)std::min((size_t)1024, avail);
            int n = http->Read(reinterpret_cast<char*>(ogg_buf.get() + ogg_len), to_read);
            if (n < 0) {
                if (frames_pushed > 10) {
                    if (current_bitrate == "16k")
                        current_bitrate = "12k";
                    else if (current_bitrate == "12k")
                        current_bitrate = "8k";
                    need_downshift = true;
                    if (current_bitrate == "8k") {
                        downshift_retries++;
                    }
                } else if (downshift_retries < kMaxRetries) {
                    need_downshift = true;
                    downshift_retries++;
                }
                break;
            }
            if (n == 0) {
                if (frames_pushed > 0 && session_bytes > 0 && eof_retries < 2 &&
                    !abort_requested_.load()) {
                    eof_retries++;
                    need_downshift = true;
                } else {
                    need_downshift = false;
                }
                break;
            }

            ogg_len += n;
            total_bytes_streamed += n;
            session_bytes += n;

            while (ogg_len >= 27) {
                if (abort_requested_.load()) {
                    break;
                }

                size_t oggs_pos = 0;
                bool found = false;
                while (oggs_pos + 4 <= ogg_len) {
                    if (memcmp(ogg_buf.get() + oggs_pos, OGGS_MAGIC, 4) == 0) {
                        found = true;
                        break;
                    }
                    oggs_pos++;
                }

                if (!found) {
                    if (ogg_len >= 3) {
                        memmove(ogg_buf.get(), ogg_buf.get() + ogg_len - 3, 3);
                        ogg_len = 3;
                    }
                    break;
                }

                if (oggs_pos > 0) {
                    memmove(ogg_buf.get(), ogg_buf.get() + oggs_pos, ogg_len - oggs_pos);
                    ogg_len -= oggs_pos;
                    if (ogg_len < 27) {
                        break;
                    }
                }

                uint8_t num_segs = ogg_buf[26];
                size_t hdr_len = 27 + num_segs;
                if (ogg_len < hdr_len) {
                    break;
                }

                size_t body_len = 0;
                for (size_t i = 0; i < num_segs; ++i) {
                    body_len += ogg_buf[27 + i];
                }
                size_t page_total = hdr_len + body_len;
                if (ogg_len < page_total) {
                    break;
                }

                size_t cur = hdr_len;
                size_t seg_idx = 0;
                while (seg_idx < num_segs) {
                    size_t pkt_len = 0;
                    size_t pkt_start = cur;
                    bool continued = false;
                    do {
                        uint8_t l = ogg_buf[27 + seg_idx++];
                        pkt_len += l;
                        cur += l;
                        continued = (l == 255);
                    } while (continued && seg_idx < num_segs);

                    if (pkt_len == 0) {
                        continue;
                    }

                    const uint8_t* pkt_ptr = ogg_buf.get() + pkt_start;

                    if (!seen_head) {
                        if (pkt_len >= 19 && memcmp(pkt_ptr, "OpusHead", 8) == 0) {
                            seen_head = true;
                        }
                        continue;
                    }
                    if (!seen_tags) {
                        if (pkt_len >= 8 && memcmp(pkt_ptr, "OpusTags", 8) == 0) {
                            seen_tags = true;
                        }
                        continue;
                    }
                    if (abort_requested_.load()) {
                        break;
                    }

                    auto packet = std::make_unique<AudioStreamPacket>();
                    auto c = Board::GetInstance().GetAudioCodec();
                    packet->sample_rate = (c != nullptr) ? c->output_sample_rate() : 24000;
                    packet->frame_duration = 60;
                    packet->payload.assign(pkt_ptr, pkt_ptr + pkt_len);

                    if (is_prebuffering) {
                        prebuffer.push_back(std::move(packet));
                        if (prebuffer.size() >= prebuffer_target) {
                            for (auto& p : prebuffer) {
                                app.GetAudioService().PushPacketToDecodeQueue(std::move(p), true);
                                frames_pushed++;
                                session_frames++;
                            }
                            prebuffer.clear();
                            is_prebuffering = false;
                        }
                    } else {
                        app.GetAudioService().PushPacketToDecodeQueue(std::move(packet), true);
                        frames_pushed++;
                        session_frames++;
                    }

                    if (session_frames >= 20) {
                        downshift_retries = 0;
                        eof_retries = 0;
                    }
                }

                memmove(ogg_buf.get(), ogg_buf.get() + page_total, ogg_len - page_total);
                ogg_len -= page_total;
            }
        }

        if (!prebuffer.empty()) {
            for (auto& p : prebuffer) {
                app.GetAudioService().PushPacketToDecodeQueue(std::move(p), true);
                frames_pushed++;
                session_frames++;
            }
            prebuffer.clear();
        }

        {
            std::lock_guard<std::mutex> lock(http_mutex_);
            active_http_ = nullptr;
        }
        http->Close();

        if (!need_downshift || abort_requested_.load()) {
            break;
        }
    }

    ESP_LOGI(TAG, "Stream done bytes=%zu frames=%d aborted=%d", total_bytes_streamed, frames_pushed,
             abort_requested_.load());

    bool was_aborted = abort_requested_.load();
    if (!was_aborted) {
        int wait_ms = 0;
        while (!app.GetAudioService().IsPlaybackIdle() && wait_ms < 5000) {
            vTaskDelay(pdMS_TO_TICKS(100));
            wait_ms += 100;
        }
    } else {
        app.GetAudioService().ResetDecoder();
    }

    is_playing_ = false;
    {
        std::lock_guard<std::mutex> lock(song_mutex_);
        current_video_id_.clear();
        current_title_.clear();
    }

    if (display != nullptr) {
        display->ShowNotification(was_aborted ? "Lagu Dihentikan" : "Lagu YouTube Selesai", 3000);
        display->SetChatMessage("assistant", "");
        display->SetChatMessage("system", "");
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
    }

    ogg_buf.reset();
    ReportStatus(was_aborted ? "stopped" : "finished", video_id);
    vTaskDelay(pdMS_TO_TICKS(150));

    app.SetDeviceState(kDeviceStateIdle);
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    vTaskDelay(pdMS_TO_TICKS(100));
    app.GetAudioService().EnableWakeWordDetection(true);
}

void YouTubeAudioPoller::TaskLoop() {
    ESP_LOGI(TAG, "YouTube player ready");
    while (running_.load()) {
        while (running_.load() && !play_requested_.load() &&
               !query_requested_.load() && !trigger_requested_.load()) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!running_.load()) {
            break;
        }

        if (play_requested_.exchange(false)) {
            std::string vid;
            std::string tit;
            {
                std::lock_guard<std::mutex> lock(song_mutex_);
                vid = pending_video_id_;
                tit = pending_title_;
            }
            if (!vid.empty()) {
                StreamAudio(vid, tit);
            }
        } else if (query_requested_.exchange(false)) {
            std::string q;
            {
                std::lock_guard<std::mutex> lock(song_mutex_);
                q = pending_query_;
            }
            if (!q.empty()) {
                is_fetching_ = true;
                auto& app = Application::GetInstance();
                bool wake = app.GetAudioService().IsWakeWordRunning();
                if (wake) {
                    app.GetAudioService().EnableWakeWordDetection(false);
                }
                std::string vid;
                std::string tit;
                bool found = FetchDirect(q, vid, tit);
                is_fetching_ = false;
                if (found) {
                    StreamAudio(vid, tit);
                } else if (wake && app.GetDeviceState() == kDeviceStateIdle) {
                    app.GetAudioService().EnableWakeWordDetection(true);
                }
            }
        } else if (trigger_requested_.exchange(false)) {
            is_fetching_ = true;
            auto& app = Application::GetInstance();
            bool wake = app.GetAudioService().IsWakeWordRunning();
            if (wake) {
                app.GetAudioService().EnableWakeWordDetection(false);
            }
            int cmd_id = 0;
            std::string video_id;
            std::string title;
            bool found = FetchCommands(cmd_id, video_id, title);
            is_fetching_ = false;
            if (found) {
                AckCommand(cmd_id);
                StreamAudio(video_id, title);
            } else if (wake && app.GetDeviceState() == kDeviceStateIdle) {
                app.GetAudioService().EnableWakeWordDetection(true);
            }
        }
    }
}
