#include "youtube_audio_poller.h"
#include <nvs.h>
#include <nvs_flash.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <cJSON.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cstring>

#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio/audio_service.h"
#include "protocols/protocol.h"
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
    std::string domain = GetServerDomain();
    if (domain.rfind("http://", 0) == 0 || domain.rfind("https://", 0) == 0) {
        return domain;
    }
    return "http://" + domain;
}

std::string YouTubeAudioPoller::GetCurrentTitle() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(song_mutex_));
    return current_title_;
}

std::string YouTubeAudioPoller::GetCurrentVideoId() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(song_mutex_));
    return current_video_id_;
}

YouTubeAudioPoller::YouTubeAudioPoller() {
}

YouTubeAudioPoller::~YouTubeAudioPoller() {
    Stop();
}

YouTubeAudioPoller& YouTubeAudioPoller::GetInstance() {
    static YouTubeAudioPoller instance;
    return instance;
}

void YouTubeAudioPoller::Start() {
    if (running_.load()) {
        return;
    }

    abort_requested_ = false;

    BaseType_t result = xTaskCreate(
        [](void* arg) {
            auto* player =
                static_cast<YouTubeAudioPoller*>(arg);

            player->TaskLoop();

            player->task_handle_ = nullptr;
            player->running_ = false;

            vTaskDelete(nullptr);
        },
        "yt_poller",
        3584,
        this,
        2,
        &task_handle_);

    if (result != pdPASS) {
        task_handle_ = nullptr;
        running_ = false;

        ESP_LOGE(
            TAG,
            "Gagal membuat task YouTube: memori task tidak cukup");

        return;
    }

    running_ = true;

    ESP_LOGI(
        TAG,
        "YouTubeAudioPoller started; stack=3584 bytes");
}

void YouTubeAudioPoller::Stop() {
    if (!running_.load()) {
        return;
    }
    running_ = false;
    abort_requested_ = true;
    if (task_handle_ != nullptr) {
        task_handle_ = nullptr;
    }
    ESP_LOGI(TAG, "YouTubeAudioPoller stopped");
}

void YouTubeAudioPoller::Abort() {
    if (is_playing_.load()) {
        ESP_LOGI(TAG, "Abort requested for YouTube audio stream");
        abort_requested_ = true;
        Application::GetInstance().GetAudioService().ResetDecoder();
        {
            std::lock_guard<std::mutex> lock(http_mutex_);
            if (active_http_ != nullptr) {
                ESP_LOGI(TAG, "Force-closing active HTTP connection to stop stream immediately");
                active_http_->Close();
            }
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
    ESP_LOGI(TAG, "PlaySong called directly: title=\"%s\", vid=%s", title.c_str(), video_id.c_str());
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
    // Guard: ignore if already playing or a play is pending (prevents TTS text triggering duplicate)
    if (is_playing_.load() || play_requested_.load()) {
        ESP_LOGW(TAG, "PlayQuery ignored (already playing or pending): \"%s\"", query.c_str());
        return;
    }
    ESP_LOGI(TAG, "PlayQuery called: query=\"%s\"", query.c_str());
    {
        std::lock_guard<std::mutex> lock(song_mutex_);
        pending_query_ = query;
    }
    query_requested_ = true;
    Application::GetInstance().GetAudioService().EnableWakeWordDetection(false);
}

static std::string ReadHttpBounded(Http* http, size_t max_bytes = 4096) {
    if (http == nullptr) return "";
    std::string body;
    char chunk[256];
    while (body.size() < max_bytes) {
        int n = http->Read(chunk, sizeof(chunk));
        if (n <= 0) break;
        body.append(chunk, n);
    }
    return body;
}

static void DrainHttp(Http* http) {
    if (http == nullptr) return;
    char drain[128];
    while (http->Read(drain, sizeof(drain)) > 0) {}
}

bool YouTubeAudioPoller::FetchDirect(const std::string& query, std::string& video_id, std::string& title) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) return false;

    auto http = network->CreateHttp(0);
    if (http == nullptr) return false;

    std::string encoded_q;
    for (char c : query) {
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded_q += c;
        } else if (c == ' ') {
            encoded_q += "%20";
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", (unsigned char)c);
            encoded_q += hex;
        }
    }

    std::string mac = SystemInfo::GetMacAddress();
    std::string url = GetBaseUrl() + "/api/audio/play_direct?q=" + encoded_q + "&mac=" + mac;
    ESP_LOGI(TAG, "FetchDirect querying URL: %s", url.c_str());
    http->SetTimeout(12000);
    http->SetHeader("User-Agent", "ESP32-XiaoZhi");
    http->SetHeader("Device-Id", mac.c_str());
    http->SetHeader("X-Device-Mac", mac.c_str());

    if (!http->Open("GET", url)) {
        ESP_LOGW(TAG, "FetchDirect failed to open: %s", url.c_str());
        http->Close();
        return false;
    }

    if (http->GetStatusCode() != 200) {
        ESP_LOGW(TAG, "FetchDirect HTTP returned %d", http->GetStatusCode());
        http->Close();
        return false;
    }

    std::string body = ReadHttpBounded(http.get(), 4096);
    http->Close();

    if (body.empty()) return false;

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) return false;

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
        ESP_LOGW(TAG, "FetchCommands: Failed to connect to %s", url.c_str());
        http->Close();
        return false;
    }

    if (http->GetStatusCode() != 200) {
        ESP_LOGW(TAG, "FetchCommands: HTTP returned status %d", http->GetStatusCode());
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
        ESP_LOGD(TAG, "Skipping status reporting to conserve internal SRAM for streaming");
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

    ESP_LOGI(TAG, "ReportStatus: reporting \"%s\" for video %s to %s", status.c_str(), video_id.c_str(), url.c_str());
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

    ESP_LOGI(TAG, ">> DETEKSI: Mulai memutar lagu YouTube: \"%s\" (vid=%s) <<", title.c_str(), video_id.c_str());
    app.GetAudioService().EnableWakeWordDetection(false);

    // Wait for TTS speaking to finish (max 8 sec) before opening stream
    // This frees MQTT audio memory and prevents audio output conflict
    {
        int wait_ms = 0;
        while (app.GetDeviceState() == kDeviceStateSpeaking && wait_ms < 8000 && !abort_requested_.load()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            wait_ms += 100;
        }
        if (wait_ms > 0) {
            ESP_LOGI(TAG, "Waited %d ms for TTS to finish before streaming", wait_ms);
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

    // Close any open audio channel to the server so mic does not record speaker output or hit 30s listening timeout
    app.CloseAudioChannel();
    app.SetDeviceState(kDeviceStateIdle);

    // Clear any pending play/query requests that accumulated during TTS wait
    play_requested_ = false;
    query_requested_ = false;

    // Report playing status now that TTS speaking is done and audio channel is closed
    ReportStatus("playing", video_id);

    std::string display_title = "[PLAY] " + title;
    if (display != nullptr) {
        display->SetChatMessage("assistant", display_title.c_str());
        display->SetStatus("Memutar Lagu");
        display->SetEmotion("music");
        display->ShowNotification(display_title.c_str(), 5000);
    }

    // Set performance mode first to stabilize CPU frequency at 160MHz before codec I2C init
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    esp_wifi_set_ps(WIFI_PS_NONE);
    vTaskDelay(pdMS_TO_TICKS(30));

    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec != nullptr) {
        if (!codec->output_enabled()) {
            codec->EnableOutput(true);
        }
        ESP_LOGI(TAG, "Audio playback using current device volume: %d%%", codec->output_volume());
    }

    // Reset decoder to clear any leftover TTS audio in the queue
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

    // 1. Initial Bitrate Evaluation based on Wi-Fi RSSI & Free Internal SRAM
    wifi_ap_record_t ap_info;
    int8_t rssi = -100;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    }
    size_t free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    // Strictly 8k - 16k range as requested for optimal ESP32-C3 stability
    std::string current_bitrate = "12k";
    if (rssi >= -68 && free_sram >= 36000) {
        current_bitrate = "16k";
    } else if (rssi >= -78 && free_sram >= 28000) {
        current_bitrate = "12k";
    } else {
        current_bitrate = "8k";
    }
    ESP_LOGI(TAG, "Adaptive Profile Selected: RSSI=%d dBm, FreeSRAM=%u B -> Bitrate=%s (Range: 8k - 16k)",
             rssi, (unsigned int)free_sram, current_bitrate.c_str());

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
        char url_buf[256];
        std::string mac = SystemInfo::GetMacAddress();
        std::string chip = CONFIG_IDF_TARGET;
        if (start_sec > 0.5f) {
            snprintf(url_buf, sizeof(url_buf), "%s/api/audio/stream/%s?br=%s&start=%.2f&mac=%s&chip=%s",
                     GetStreamUrl().c_str(), video_id.c_str(), current_bitrate.c_str(), start_sec, mac.c_str(), chip.c_str());
        } else {
            snprintf(url_buf, sizeof(url_buf), "%s/api/audio/stream/%s?br=%s&mac=%s&chip=%s",
                     GetStreamUrl().c_str(), video_id.c_str(), current_bitrate.c_str(), mac.c_str(), chip.c_str());
        }
        std::string stream_url(url_buf);

        http->SetTimeout(15000); // 15s timeout to tolerate network jitter & backend transcoding latency
        http->SetHeader("User-Agent", "ESP32-XiaoZhi");
        http->SetHeader("Device-Id", mac.c_str());
        http->SetHeader("X-Device-Mac", mac.c_str());
        http->SetHeader("X-Device-Chip", chip.c_str());
        http->SetHeader("Device-Chip", chip.c_str());

        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        esp_wifi_set_ps(WIFI_PS_NONE);
        ESP_LOGI(TAG, "Connecting to stream URL: %s", stream_url.c_str());
        if (!http->Open("GET", stream_url)) {
            ESP_LOGE(TAG, "Failed to open stream URL: %s", stream_url.c_str());
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
            ESP_LOGE(TAG, "Stream returned HTTP %d", http->GetStatusCode());
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
        ESP_LOGI(TAG, "Stream connected (HTTP 200, br=%s, start=%.2fs), reading chunks...",
                 current_bitrate.c_str(), start_sec);

        size_t ogg_len = 0;
        bool seen_head = false;
        bool seen_tags = false;
        int sample_rate = 24000;
        bool need_downshift = false;
        int64_t last_audio_active_us = esp_timer_get_time();
        size_t session_bytes = 0;
        int session_frames = 0;

        // Dynamic Pre-Buffer (Jitter Buffer):
        // 4 frames (~240ms) on good signal, 6 frames (~360ms) on weaker signal or reconnect
        size_t prebuffer_target = (live_rssi >= -75 && frames_pushed == 0) ? 4 : 6;
        std::vector<std::unique_ptr<AudioStreamPacket>> prebuffer;
        bool is_prebuffering = true;

        try {
            while (running_.load() && !abort_requested_.load()) {
                int64_t now_us = esp_timer_get_time();
                bool playback_idle = app.GetAudioService().IsPlaybackIdle();

                // Check live Wi-Fi signal periodically without lag
                if (now_us - last_rssi_check_us > 5000000) {
                    last_rssi_check_us = now_us;
                    wifi_ap_record_t live_ap;
                    if (esp_wifi_sta_get_ap_info(&live_ap) == ESP_OK) {
                        live_rssi = live_ap.rssi;
                    }
                }

                // 1. Speaker Starvation Watchdog (Fast Drop):
                if (frames_pushed > 10) {
                    if (!playback_idle) {
                        last_audio_active_us = now_us;
                    } else if (now_us - last_audio_active_us > 2500000) { // 2.5s speaker silence
                        ESP_LOGW(TAG, "Audio starvation detected! Speaker is silent at br=%s, auto-recovering...", current_bitrate.c_str());
                        if (current_bitrate == "16k") current_bitrate = "12k";
                        else if (current_bitrate == "12k") current_bitrate = "8k";
                        else current_bitrate = "8k";

                        need_downshift = true;
                        if (current_bitrate == "8k") downshift_retries++;
                        ESP_LOGW(TAG, "Watchdog recovered stream by downshifting/reconnecting to %s at %.2fs",
                                 current_bitrate.c_str(), frames_pushed * 0.060f);
                        break;
                    }
                }

                size_t avail = kOggBufCap - ogg_len;
                if (avail == 0) {
                    // Buffer is full without a parsed page. Discard until next OggS magic.
                    size_t next_oggs = 1;
                    while (next_oggs + 4 <= ogg_len && memcmp(ogg_buf.get() + next_oggs, OGGS_MAGIC, 4) != 0) {
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
                    ESP_LOGW(TAG, "HTTP read error or stream closed (ret=%d, total=%u, frames=%d)",
                             n, (unsigned int)total_bytes_streamed, frames_pushed);
                    if (frames_pushed > 10) {
                        if (current_bitrate == "16k") current_bitrate = "12k";
                        else if (current_bitrate == "12k") current_bitrate = "8k";
                        else current_bitrate = "8k";

                        need_downshift = true;
                        if (current_bitrate == "8k") downshift_retries++;
                        ESP_LOGW(TAG, "Connection interrupted mid-stream! Downshifting to %s at %.2fs",
                                 current_bitrate.c_str(), frames_pushed * 0.060f);
                    } else if (downshift_retries < kMaxRetries) {
                        need_downshift = true;
                        downshift_retries++;
                    }
                    break;
                }
                if (n == 0) {
                    ESP_LOGI(TAG, "Stream EOF reached (session_bytes=%u, session_frames=%d, total=%u, frames_pushed=%d)",
                             (unsigned int)session_bytes, session_frames, (unsigned int)total_bytes_streamed, frames_pushed);
                    // YouTube CDN sometimes drops TCP prematurely after 1-2 minutes.
                    // Auto-resume from offset start_sec if frames were pushed.
                    if (frames_pushed > 0 && session_bytes > 0 && eof_retries < 2 && !abort_requested_.load()) {
                        eof_retries++;
                        need_downshift = true;
                        ESP_LOGI(TAG, "Potential premature EOF from YouTube/CDN! Auto-resuming at %.2fs (probe %d/2)...",
                                 frames_pushed * 0.060f, eof_retries);
                    } else {
                        if (session_bytes == 0 && frames_pushed > 0) {
                            ESP_LOGI(TAG, "Confirmed end of audio stream at %.2fs", frames_pushed * 0.060f);
                        }
                        need_downshift = false;
                    }
                    break;
                }

                ogg_len += n;
                total_bytes_streamed += n;
                session_bytes += n;

                // Demux Ogg pages incrementally
                while (ogg_len >= 27) {
                    if (abort_requested_.load()) {
                        ESP_LOGI(TAG, "Audio stream aborted by user request");
                        break;
                    }

                    // Find next OggS magic
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
                        if (ogg_len < 27) break;
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

                    // Extract packets from this page
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
                                if (pkt_len >= 16) {
                                    sample_rate = pkt_ptr[12] | (pkt_ptr[13] << 8) | (pkt_ptr[14] << 16) | (pkt_ptr[15] << 24);
                                    ESP_LOGI(TAG, "OpusHead: sample_rate=%d", sample_rate);
                                }
                            }
                            continue;
                        }

                        if (!seen_tags) {
                            if (pkt_len >= 8 && memcmp(pkt_ptr, "OpusTags", 8) == 0) {
                                seen_tags = true;
                                ESP_LOGI(TAG, "OpusTags parsed, ready to decode audio");
                            }
                            continue;
                        }

                        if (abort_requested_.load()) {
                            ESP_LOGI(TAG, "Audio stream aborted by user request");
                            break;
                        }

                        // Push raw Opus frame (decode directly at codec output sample rate: 16kHz)
                        auto packet = std::make_unique<AudioStreamPacket>();
                        auto codec = Board::GetInstance().GetAudioCodec();
                        packet->sample_rate = (codec != nullptr) ? codec->output_sample_rate() : 16000;
                        packet->frame_duration = 60;
                        packet->payload.assign(pkt_ptr, pkt_ptr + pkt_len);

                        if (is_prebuffering) {
                            prebuffer.push_back(std::move(packet));
                            if (prebuffer.size() >= prebuffer_target) {
                                ESP_LOGI(TAG, "Pre-buffer ready (%u frames, ~%u ms), flushing to decoder (br=%s)",
                                         (unsigned int)prebuffer.size(), (unsigned int)(prebuffer.size() * 60), current_bitrate.c_str());
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
                            if (frames_pushed == 1 || frames_pushed % 50 == 0) {
                                ESP_LOGI(TAG, "Audio frame #%d pushed to decoder (len=%u)",
         frames_pushed, (unsigned int)pkt_len);
                            }
                        }

                        if (session_frames >= 20) {
                            downshift_retries = 0;
                            eof_retries = 0;
                        }
                    }

                    memmove(ogg_buf.get(), ogg_buf.get() + page_total, ogg_len - page_total);
                    ogg_len -= page_total;

                    if (abort_requested_.load()) {
                        ESP_LOGI(TAG, "Audio stream aborted by user request");
                        break;
                    }
                }
            }
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "StreamAudio exception: %s", e.what());
        }

        // Flush remaining prebuffer if stream interrupted before reaching prebuffer target
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
    ESP_LOGI(TAG, "Stream finished (total %zu bytes read, frames_pushed=%d, aborted=%d)",
             total_bytes_streamed, frames_pushed, abort_requested_.load());

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

    // Set is_playing_ to false immediately so device state logic knows playback has ended
    is_playing_ = false;
    {
        std::lock_guard<std::mutex> lock(song_mutex_);
        current_video_id_.clear();
        current_title_.clear();
    }

    if (!was_aborted) {
        ESP_LOGI(TAG, ">> DETEKSI: Lagu YouTube \"%s\" SELESAI DIPUTAR secara normal <<", title.c_str());
        if (display != nullptr) {
            display->ShowNotification("Lagu YouTube Selesai", 3000);
            display->SetChatMessage("assistant", "");
            display->SetChatMessage("system", "");
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
        }
    } else {
        ESP_LOGI(TAG, ">> DETEKSI: Lagu YouTube \"%s\" DIHENTIKAN <<", title.c_str());
        if (display != nullptr) {
            display->ShowNotification("Lagu Dihentikan", 3000);
            display->SetChatMessage("assistant", "");
            display->SetChatMessage("system", "");
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
        }
    }

    // Release the 3KB OGG buffer immediately so heap contiguous memory expands
    ogg_buf.reset();

    // 1. Report playback end status via lightweight HTTP while WakeNet is not loaded (free SRAM > 35KB)
    std::string end_status = was_aborted ? "stopped" : "finished";
    ReportStatus(end_status, video_id);
    vTaskDelay(pdMS_TO_TICKS(150));

    // 2. Cleanly restore device state to Idle/Standby
    app.SetDeviceState(kDeviceStateIdle);
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    vTaskDelay(pdMS_TO_TICKS(100));

    // 3. Re-enable WakeWord detection in clean, unfragmented memory
    app.GetAudioService().EnableWakeWordDetection(true);
}

void YouTubeAudioPoller::TaskLoop() {
    ESP_LOGI(TAG, "YouTube player ready (on-demand direct playback)");

    while (running_.load()) {
        // Wait for direct tool call (play_requested_), direct query (query_requested_), or trigger (trigger_requested_)
        while (running_.load() && !play_requested_.load() && !query_requested_.load() && !trigger_requested_.load()) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        if (!running_.load()) {
            break;
        }

        // Direct MCP tool call has highest priority
        if (play_requested_.exchange(false)) {
            std::string vid, tit;
            {
                std::lock_guard<std::mutex> lock(song_mutex_);
                vid = pending_video_id_;
                tit = pending_title_;
            }
            if (!vid.empty()) {
                ESP_LOGI(TAG, "Direct tool call playing: \"%s\" (vid=%s)", tit.c_str(), vid.c_str());
                StreamAudio(vid, tit);
            }
        }
        // Direct voice query search
        else if (query_requested_.exchange(false)) {
            std::string q;
            {
                std::lock_guard<std::mutex> lock(song_mutex_);
                q = pending_query_;
            }
            if (!q.empty()) {
                is_fetching_ = true;
                auto& app = Application::GetInstance();
                bool was_wake_word = app.GetAudioService().IsWakeWordRunning();
                if (was_wake_word) {
                    ESP_LOGI(TAG, "Temporarily stopping WakeNet to free SRAM for HTTP query");
                    app.GetAudioService().EnableWakeWordDetection(false);
                }

                ESP_LOGI(TAG, "Searching & streaming song query: \"%s\"", q.c_str());
                std::string vid, tit;
                bool found = FetchDirect(q, vid, tit);
                is_fetching_ = false;

                if (found) {
                    StreamAudio(vid, tit);
                } else if (was_wake_word && app.GetDeviceState() == kDeviceStateIdle) {
                    app.GetAudioService().EnableWakeWordDetection(true);
                }
            }
        }
        // Fallback queue trigger
        else if (trigger_requested_.exchange(false)) {
            is_fetching_ = true;
            auto& app = Application::GetInstance();
            bool was_wake_word = app.GetAudioService().IsWakeWordRunning();
            if (was_wake_word) {
                ESP_LOGI(TAG, "Temporarily stopping WakeNet to free SRAM for command fetch");
                app.GetAudioService().EnableWakeWordDetection(false);
            }

            int cmd_id = 0;
            std::string video_id;
            std::string title;
            bool found = FetchCommands(cmd_id, video_id, title);
            is_fetching_ = false;

            if (found) {
                ESP_LOGI(TAG, "Playing requested YouTube song (1 song only): id=%d, title=\"%s\", vid=%s",
                         cmd_id, title.c_str(), video_id.c_str());
                AckCommand(cmd_id);
                StreamAudio(video_id, title);
            } else if (was_wake_word && app.GetDeviceState() == kDeviceStateIdle) {
                app.GetAudioService().EnableWakeWordDetection(true);
            }
        }
    }
}
