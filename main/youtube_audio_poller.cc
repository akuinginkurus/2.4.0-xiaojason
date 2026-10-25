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
    while (!domain.empty() && domain.back() == '/') {
        domain.pop_back();
    }
    // Always force plain HTTP (Port 80) on ESP32-C3 to prevent mbedTLS SSL handshake
    // from consuming 30KB+ SRAM and triggering Out-Of-Memory aborts.
    if (domain.rfind("https://", 0) == 0) {
        domain = domain.substr(8);
    } else if (domain.rfind("http://", 0) == 0) {
        domain = domain.substr(7);
    }
    // Remove :443 if user configured port 443
    size_t colon_pos = domain.rfind(":443");
    if (colon_pos != std::string::npos && colon_pos == domain.length() - 4) {
        domain = domain.substr(0, colon_pos);
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

std::string YouTubeAudioPoller::GetTotalDuration() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(song_mutex_));
    return total_duration_str_;
}

int YouTubeAudioPoller::GetTotalDurationSec() const {
    return total_duration_sec_;
}

std::string YouTubeAudioPoller::GetCurrentDuration() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(song_mutex_));
    return current_duration_str_;
}

int YouTubeAudioPoller::GetCurrentDurationSec() const {
    return current_duration_sec_.load();
}

int YouTubeAudioPoller::ParseDurationToSeconds(const std::string& dur_str) {
    if (dur_str.empty() || dur_str == "--:--") {
        return 0;
    }
    bool all_digits = true;
    for (char c : dur_str) {
        if (!isdigit((unsigned char)c)) {
            all_digits = false;
            break;
        }
    }
    if (all_digits) {
        return atoi(dur_str.c_str());
    }

    int part1 = 0, part2 = 0, part3 = 0;
    int colons = 0;
    for (char c : dur_str) {
        if (c == ':') colons++;
    }

    if (colons == 1) {
        if (sscanf(dur_str.c_str(), "%d:%d", &part1, &part2) == 2) {
            return part1 * 60 + part2;
        }
    } else if (colons >= 2) {
        if (sscanf(dur_str.c_str(), "%d:%d:%d", &part1, &part2, &part3) == 3) {
            return part1 * 3600 + part2 * 60 + part3;
        }
    }
    return 0;
}

std::string YouTubeAudioPoller::FormatSecondsToDuration(int seconds) {
    if (seconds < 0) {
        seconds = 0;
    }
    char buf[16];
    if (seconds >= 3600) {
        int h = seconds / 3600;
        int m = (seconds % 3600) / 60;
        int s = seconds % 60;
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
    } else {
        int m = seconds / 60;
        int s = seconds % 60;
        snprintf(buf, sizeof(buf), "%02d:%02d", m, s);
    }
    return std::string(buf);
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
    running_ = true;
    abort_requested_ = false;

    xTaskCreate([](void* arg) {
        static_cast<YouTubeAudioPoller*>(arg)->TaskLoop();
        vTaskDelete(NULL);
    }, "yt_poller", 3584, this, 2, &task_handle_);
    ESP_LOGI(TAG, "YouTubeAudioPoller started");
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
    ESP_LOGI(TAG, "Abort requested for YouTube audio stream");
    abort_requested_ = true;
    play_requested_ = false;
    query_requested_ = false;
    trigger_requested_ = false;
    is_fetching_ = false;
    Application::GetInstance().GetAudioService().ResetDecoder();
    {
        std::lock_guard<std::mutex> lock(http_mutex_);
        if (active_http_ != nullptr) {
            ESP_LOGI(TAG, "Force-closing active HTTP connection to stop stream immediately");
            active_http_->Close();
        }
    }
}

void YouTubeAudioPoller::TriggerFetch() {
    ESP_LOGI(TAG, "Fetch triggered externally");
    trigger_requested_ = true;
    Application::GetInstance().GetAudioService().EnableWakeWordDetection(false);
}

void YouTubeAudioPoller::PlaySong(const std::string& video_id, const std::string& title, const std::string& duration) {
    if (video_id.empty()) {
        return;
    }
    ESP_LOGI(TAG, "PlaySong called directly: title=\"%s\", vid=%s, duration=\"%s\"",
             title.c_str(), video_id.c_str(), duration.c_str());
    {
        std::lock_guard<std::mutex> lock(song_mutex_);
        pending_video_id_ = video_id;
        pending_title_ = title.empty() ? "YouTube Song" : title;
        pending_duration_ = duration;
    }
    play_requested_ = true;
    Application::GetInstance().GetAudioService().EnableWakeWordDetection(false);
}

void YouTubeAudioPoller::PlayQuery(const std::string& query) {
    CheckAndConfirmQuery(query);
}

void YouTubeAudioPoller::CheckAndConfirmQuery(const std::string& query) {
    if (query.empty()) {
        return;
    }
    // Guard: ignore if already playing or a play is pending
    if (is_playing_.load() || play_requested_.load()) {
        ESP_LOGW(TAG, "CheckAndConfirmQuery ignored (already playing or pending): \"%s\"", query.c_str());
        return;
    }
    ESP_LOGI(TAG, "CheckAndConfirmQuery called: query=\"%s\"", query.c_str());
    {
        std::lock_guard<std::mutex> lock(song_mutex_);
        pending_query_ = query;
    }
    query_requested_ = true;
    Application::GetInstance().GetAudioService().EnableWakeWordDetection(false);
}

void YouTubeAudioPoller::CheckAndConfirmSong(const std::string& video_id, const std::string& title) {
    if (!video_id.empty()) {
        CheckAndConfirmQuery(video_id);
    } else if (!title.empty()) {
        CheckAndConfirmQuery(title);
    }
}

bool YouTubeAudioPoller::SearchSong(const std::string& query, std::string& video_id, std::string& title, std::string& duration) {
    if (query.empty()) {
        return false;
    }
    ESP_LOGI(TAG, "SearchSong called: query=\"%s\"", query.c_str());
    bool found = FetchDirect(query, video_id, title, duration);
    if (found) {
        ESP_LOGI(TAG, ">> DETEKSI: Lagu YouTube ditemukan: \"%s\" (durasi: %s, vid: %s) <<",
                 title.c_str(), duration.c_str(), video_id.c_str());
        Application::GetInstance().ArmSongConfirmation(video_id, title, duration);
    } else {
        ESP_LOGW(TAG, ">> DETEKSI: Lagu YouTube tidak ditemukan: \"%s\" <<", query.c_str());
    }
    return found;
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
    std::string duration;
    return FetchDirect(query, video_id, title, duration);
}

bool YouTubeAudioPoller::FetchDirect(const std::string& query, std::string& video_id, std::string& title, std::string& duration) {
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
    cJSON* dur_item = cJSON_GetObjectItem(root, "duration");
    if (cJSON_IsString(vid_item)) {
        video_id = vid_item->valuestring;
        title = cJSON_IsString(title_item) ? title_item->valuestring : query;
        if (cJSON_IsString(dur_item)) {
            duration = dur_item->valuestring;
        } else if (cJSON_IsNumber(dur_item)) {
            int dur_sec = dur_item->valueint;
            char dur_buf[16];
            snprintf(dur_buf, sizeof(dur_buf), "%02d:%02d", dur_sec / 60, dur_sec % 60);
            duration = dur_buf;
        } else {
            duration = "--:--";
        }
        found = true;
    }
    cJSON_Delete(root);
    return found;
}

bool YouTubeAudioPoller::FetchCommands(int& cmd_id, std::string& video_id, std::string& title) {
    std::string duration;
    return FetchCommands(cmd_id, video_id, title, duration);
}

bool YouTubeAudioPoller::FetchCommands(int& cmd_id, std::string& video_id, std::string& title, std::string& duration) {
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
            cJSON* dur_item = cJSON_GetObjectItem(cmd, "duration");

            if ((cJSON_IsNumber(id_item) || cJSON_IsString(id_item)) && cJSON_IsString(vid_item)) {
                cmd_id = cJSON_IsNumber(id_item) ? id_item->valueint : atoi(id_item->valuestring);
                video_id = vid_item->valuestring;
                title = cJSON_IsString(title_item) ? title_item->valuestring : "YouTube Song";
                if (cJSON_IsString(dur_item)) {
                    duration = dur_item->valuestring;
                } else if (cJSON_IsNumber(dur_item)) {
                    duration = FormatSecondsToDuration(dur_item->valueint);
                } else {
                    duration = "--:--";
                }
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


void YouTubeAudioPoller::StreamAudio(const std::string& video_id, const std::string& title, const std::string& duration) {
    auto& app = Application::GetInstance();
    auto display = Board::GetInstance().GetDisplay();

    {
        std::lock_guard<std::mutex> lock(song_mutex_);
        current_video_id_ = video_id;
        current_title_ = title;
        current_duration_str_ = "00:00";
        total_duration_str_ = duration.empty() ? "--:--" : duration;
        total_duration_sec_ = ParseDurationToSeconds(total_duration_str_);
        if (total_duration_sec_ > 0) {
            total_duration_str_ = FormatSecondsToDuration(total_duration_sec_);
        }
        current_duration_sec_ = 0;
    }
    is_playing_ = true;
    abort_requested_ = false;
    app.ResetAborted();

    ESP_LOGI(TAG, ">> DETEKSI: Mulai memutar lagu YouTube: \"%s\" (vid=%s) <<", title.c_str(), video_id.c_str());
    ESP_LOGI(TAG, ">> DETEKSI: Total durasi lagu: %s (%d detik), Durasi awal: 00:00 <<",
             total_duration_str_.c_str(), total_duration_sec_);
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

    // Keep UI work minimal while streaming: the internal heap of the ESP32-C3 is
    // extremely tight and LVGL hangs (watchdog) if one of its allocations fails.
    // No long song title, no emotion change, only a short static status.
    if (display != nullptr) {
        display->SetChatMessage("assistant", "");
        display->SetStatus("Memutar lagu");
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

    // Prioritaskan bitrate 16k untuk kualitas audio jernih dan lancar seperti semula
    std::string current_bitrate = "16k";
    if (rssi < -78 || free_sram < 26000) {
        current_bitrate = "12k";
    }
    ESP_LOGI(TAG, "Adaptive Profile Selected: RSSI=%d dBm, FreeSRAM=%u B -> Bitrate=%s (Original High Quality)",
             rssi, (unsigned int)free_sram, current_bitrate.c_str());

    static const uint8_t OGGS_MAGIC[4] = {'O', 'g', 'g', 'S'};
    static const size_t kOggBufCap = 2048;
    // Stop streaming (instead of freezing the whole device) when internal heap gets this low
    static const size_t kLowHeapAbortBytes = 9000;
    auto ogg_buf = std::make_unique<uint8_t[]>(kOggBufCap);

    size_t total_bytes_streamed = 0;
    int frames_pushed = 0;
    int downshift_retries = 0;
    int eof_retries = 0;
    static const int kMaxRetries = 3;
    int64_t last_rssi_check_us = 0;
    int8_t live_rssi = rssi;

    int last_status_sec = -1;
    auto update_duration_progress = [&](int frames) {
        int cur_sec = (int)(frames * 0.060f);
        current_duration_sec_ = cur_sec;
        if (cur_sec != last_status_sec) {
            last_status_sec = cur_sec;
            std::string cur_str = FormatSecondsToDuration(cur_sec);
            {
                std::lock_guard<std::mutex> lock(song_mutex_);
                current_duration_str_ = cur_str;
            }
            // Redraw the OLED only every 10 s to keep LVGL allocations rare
            if (display != nullptr && cur_sec % 10 == 0) {
                std::string status_str = cur_str + " / " + total_duration_str_;
                display->SetStatus(status_str.c_str());
            }
            // Heap trend log (send these lines back if the device still hangs)
            if (cur_sec % 2 == 0) {
                ESP_LOGI(TAG, "HEAP @%s free=%u largest=%u min=%u", cur_str.c_str(),
                         (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                         (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                         (unsigned int)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            }
            if (cur_sec == 1 || cur_sec % 5 == 0) {
                if (total_duration_sec_ > 0) {
                    float pct = ((float)cur_sec / (float)total_duration_sec_) * 100.0f;
                    if (pct > 100.0f) pct = 100.0f;
                    ESP_LOGI(TAG, ">> DETEKSI: Durasi saat ini: %s / %s (%.1f%%) <<",
                             cur_str.c_str(), total_duration_str_.c_str(), pct);
                } else {
                    ESP_LOGI(TAG, ">> DETEKSI: Durasi saat ini: %s / %s <<",
                             cur_str.c_str(), total_duration_str_.c_str());
                }
            }
        }
    };

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

                // Safety net: stop the stream before the heap is completely exhausted.
                // Otherwise LVGL fails an allocation, spins forever and the whole device freezes.
                if (frames_pushed > 0 &&
                    heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < kLowHeapAbortBytes) {
                    ESP_LOGE(TAG, "Internal heap critically low (%u B free), stopping stream to avoid a system freeze",
                             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
                    abort_requested_ = true;
                    break;
                }

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
                    } else if (now_us - last_audio_active_us > 3500000) { // 3.5s speaker silence tolerates jitter without premature drop
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
                                update_duration_progress(frames_pushed);
                            }
                        } else {
                            app.GetAudioService().PushPacketToDecodeQueue(std::move(packet), true);
                            frames_pushed++;
                            session_frames++;
                            update_duration_progress(frames_pushed);
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
            update_duration_progress(frames_pushed);
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
    bool playback_succeeded = (frames_pushed > 10 && total_bytes_streamed > 2048);

    if (playback_succeeded && !was_aborted) {
        ESP_LOGI(TAG, ">> DETEKSI: Stream audio selesai diunduh. Menunggu speaker memainkan sisa buffer audio... <<");
        int wait_ms = 0;
        while (!app.GetAudioService().IsPlaybackIdle() && wait_ms < 6000 && !abort_requested_.load()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            wait_ms += 100;
            update_duration_progress(frames_pushed);
        }
    } else {
        app.GetAudioService().ResetDecoder();
    }

    was_aborted = abort_requested_.load();
    is_playing_ = false;

    int final_sec = (int)(frames_pushed * 0.060f);
    std::string final_dur_str = FormatSecondsToDuration(final_sec);
    current_duration_sec_ = final_sec;

    {
        std::lock_guard<std::mutex> lock(song_mutex_);
        current_video_id_.clear();
        current_title_.clear();
        current_duration_str_ = final_dur_str;
    }

    if (playback_succeeded && !was_aborted) {
        ESP_LOGI(TAG, ">> DETEKSI: Lagu YouTube \"%s\" SELESAI DIPUTAR secara normal (Durasi: %s / %s, Total Frames: %d) <<",
                 title.c_str(), final_dur_str.c_str(), total_duration_str_.c_str(), frames_pushed);
        if (display != nullptr) {
            display->ShowNotification("Lagu Selesai, Mendengarkan...", 3000);
            display->SetChatMessage("assistant", "");
            display->SetChatMessage("system", "");
            display->SetStatus("Mendengarkan...");
            display->SetEmotion("neutral");
        }
    } else if (was_aborted) {
        ESP_LOGI(TAG, ">> DETEKSI: Lagu YouTube \"%s\" DIHENTIKAN (Durasi: %s / %s) <<",
                 title.c_str(), final_dur_str.c_str(), total_duration_str_.c_str());
        if (display != nullptr) {
            display->ShowNotification("Lagu Dihentikan", 3000);
            display->SetChatMessage("assistant", "");
            display->SetChatMessage("system", "");
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
        }
    } else {
        ESP_LOGW(TAG, ">> DETEKSI: Gagal streaming lagu YouTube \"%s\" (frames=%d, bytes=%zu) <<",
                 title.c_str(), frames_pushed, total_bytes_streamed);
        if (display != nullptr) {
            display->ShowNotification("Gagal Memutar Lagu", 3000);
            display->SetChatMessage("assistant", "Gagal memutar lagu dari YouTube.");
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
        }
    }

    // Release the 3KB OGG buffer immediately so heap contiguous memory expands
    ogg_buf.reset();

    // 1. Report playback end status via lightweight HTTP while WakeNet is not loaded (free SRAM > 35KB)
    std::string end_status = was_aborted ? "stopped" : (playback_succeeded ? "finished" : "failed");
    ReportStatus(end_status, video_id);
    vTaskDelay(pdMS_TO_TICKS(100));

    if (playback_succeeded && !was_aborted) {
        // Otomatis langsung masuk ke mode LISTENING HANYA jika lagu benar-benar sudah selesai diputar!
        ESP_LOGI(TAG, ">> DETEKSI: Lagu selesai diputar -> LANGSUNG MASUK KE MODE LISTENING <<");
        app.Schedule([&app]() {
            app.EnterListeningModeAfterSong();
        });
    } else {
        // 2. Cleanly restore device state to Idle/Standby
        app.SetDeviceState(kDeviceStateIdle);
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        vTaskDelay(pdMS_TO_TICKS(100));

        // 3. Re-enable WakeWord detection in clean, unfragmented memory
        app.GetAudioService().EnableWakeWordDetection(true);
    }
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

        // Direct MCP tool call or confirmed song playback has highest priority
        if (play_requested_.exchange(false)) {
            std::string vid, tit, dur;
            {
                std::lock_guard<std::mutex> lock(song_mutex_);
                vid = pending_video_id_;
                tit = pending_title_;
                dur = pending_duration_;
            }
            if (!vid.empty()) {
                ESP_LOGI(TAG, "Playing confirmed song: \"%s\" (vid=%s, dur=%s)", tit.c_str(), vid.c_str(), dur.c_str());
                StreamAudio(vid, tit, dur);
            }
        }
        // Direct voice query search / checking with user confirmation
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

                ESP_LOGI(TAG, ">> DETEKSI: Mengecek info lagu YouTube: \"%s\" <<", q.c_str());
                std::string vid, tit, dur;
                bool found = FetchDirect(q, vid, tit, dur);
                is_fetching_ = false;

                if (found) {
                    ESP_LOGI(TAG, ">> DETEKSI: Lagu YouTube ditemukan: \"%s\" (durasi: %s, vid: %s). Meminta konfirmasi user. <<",
                             tit.c_str(), dur.c_str(), vid.c_str());
                    app.Schedule([&app, vid, tit, dur]() {
                        app.RequestSongConfirmation(vid, tit, dur);
                    });
                } else {
                    ESP_LOGW(TAG, ">> DETEKSI: Lagu YouTube tidak ditemukan: \"%s\" <<", q.c_str());
                    app.Schedule([&app]() {
                        app.OnSongNotFound();
                    });
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
            std::string duration;
            bool found = FetchCommands(cmd_id, video_id, title, duration);
            is_fetching_ = false;

            if (found) {
                ESP_LOGI(TAG, "Playing requested YouTube song (1 song only): id=%d, title=\"%s\", vid=%s, dur=%s",
                         cmd_id, title.c_str(), video_id.c_str(), duration.c_str());
                AckCommand(cmd_id);
                StreamAudio(video_id, title, duration);
            } else if (was_wake_word && app.GetDeviceState() == kDeviceStateIdle) {
                app.GetAudioService().EnableWakeWordDetection(true);
            }
        }
    }
}
