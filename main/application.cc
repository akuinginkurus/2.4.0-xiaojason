#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"
#include "mode/mode_store.h"
#include "youtube_audio_poller.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <esp_err.h>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>
#include <initializer_list>
#include <nvs.h>
#include <vector>

#define TAG "Application"

namespace {

std::string ToLowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

int ParseLocalSongIndex(const std::string& lowered_text) {
    (void)lowered_text;
    return 0;
}

bool IsLocalSongStopCommand(const std::string& lowered_text) {
    auto has = [&lowered_text](const char* token) {
        return lowered_text.find(token) != std::string::npos;
    };
    return has("stop") || has("berhenti") || has("hentikan") || has("henti") ||
           has("pause") || has("cukup") || has("matikan lagu") || has("matikan musik") ||
           has("stop lagu") || has("stop musik") || has("jangan putar");
}

bool ContainsToken(const std::string& lowered_text, std::initializer_list<const char*> tokens) {
    for (const char* token : tokens) {
        if (lowered_text.find(token) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool ContainsNormalizedPhrase(const std::string& normalized_text, const char* phrase) {
    std::string token(phrase);
    if (token.empty()) {
        return false;
    }
    size_t pos = normalized_text.find(token);
    while (pos != std::string::npos) {
        bool left_ok = (pos == 0) || (normalized_text[pos - 1] == ' ');
        size_t end = pos + token.size();
        bool right_ok = (end == normalized_text.size()) || (normalized_text[end] == ' ');
        if (left_ok && right_ok) {
            return true;
        }
        pos = normalized_text.find(token, pos + 1);
    }
    return false;
}

std::string NormalizeCommandText(const std::string& text) {
    std::string normalized;
    normalized.reserve(text.size());

    bool prev_space = true;
    for (unsigned char ch : text) {
        if (std::isalnum(ch)) {
            normalized.push_back(static_cast<char>(std::tolower(ch)));
            prev_space = false;
            continue;
        }
        if (!prev_space) {
            normalized.push_back(' ');
            prev_space = true;
        }
    }

    if (!normalized.empty() && normalized.back() == ' ') {
        normalized.pop_back();
    }
    return normalized;
}

bool IsWifiResetQuickConfirmCommand(const std::string& normalized_text) {
    return normalized_text == "ya" ||
           normalized_text == "iya" ||
           normalized_text == "yes" ||
           normalized_text.rfind("ya ", 0) == 0 ||
           normalized_text.rfind("iya ", 0) == 0 ||
           normalized_text.rfind("yes ", 0) == 0;
}

bool IsWifiResetCommand(const std::string& lowered_text) {
    return ContainsToken(lowered_text, {
        "reset wifi",
        "reset wi-fi",
        "ganti wifi",
        "ganti wi-fi",
        "ubah wifi",
        "ubah ssid",
        "change wifi",
        "hapus wifi",
        "hapus wi-fi",
        "hapus ssid",
        "lupa wifi",
        "clear wifi",
        "forget wifi",
        "reset ssid",
        "clear ssid"
    });
}

bool IsWifiResetConfirmCommand(const std::string& lowered_text) {
    return ContainsToken(lowered_text, {
        "konfirmasi reset wifi",
        "konfirmasi hapus wifi",
        "konfirmasi penghapusan",
        "confirm reset wifi",
        "yes reset wifi",
        "ya reset wifi",
        "iya reset wifi",
        "ya hapus wifi",
        "iya hapus wifi",
        "yes hapus wifi",
        "hapus wifi sekarang",
        "reset wifi sekarang",
        "konfirmasi reset ssid"
    });
}

bool IsWifiResetCancelCommand(const std::string& lowered_text) {
    return ContainsToken(lowered_text, {
        "batal reset wifi",
        "batalkan reset wifi",
        "cancel reset wifi"
    });
}

bool IsStandbyCommand(const std::string& normalized_text) {
    return ContainsNormalizedPhrase(normalized_text, "stop") ||
           ContainsNormalizedPhrase(normalized_text, "udahan dulu") ||
           ContainsNormalizedPhrase(normalized_text, "byee") ||
           ContainsNormalizedPhrase(normalized_text, "bye") ||
           ContainsNormalizedPhrase(normalized_text, "sampai jumpa") ||
           ContainsNormalizedPhrase(normalized_text, "sampai nanti");
}

bool IsModeSwitchCommand(const std::string& normalized_text) {
    // Avoid switching when the recognized sentence explicitly negates/cancels it.
    if (ContainsNormalizedPhrase(normalized_text, "jangan") ||
        ContainsNormalizedPhrase(normalized_text, "tidak") ||
        ContainsNormalizedPhrase(normalized_text, "gak") ||
        ContainsNormalizedPhrase(normalized_text, "nggak") ||
        ContainsNormalizedPhrase(normalized_text, "batal")) {
        return false;
    }

    static constexpr const char* kModeCommands[] = {
        "pindah mode",
        "ganti mode",
        "alihkan mode",
        "ubah mode",
        "switch mode",
        "pindah ke chronchi",
        "ganti ke chronchi",
        "alihkan ke chronchi",
        "masuk mode chronchi",
        "mode chronchi",
        "pindah ke xiaozhi",
        "ganti ke xiaozhi",
        "alihkan ke xiaozhi",
        "masuk mode xiaozhi",
        "mode xiaozhi",
        "kembali ke xiaozhi",
        "balik ke xiaozhi",
        "kembali ke awal",
        "mode normal",
    };
    for (const char* command : kModeCommands) {
        if (ContainsNormalizedPhrase(normalized_text, command)) {
            return true;
        }
    }
    return false;
}

constexpr int64_t kWifiResetConfirmWindowUs = 15LL * 1000000LL;
constexpr int64_t kWakeWordStartupSuppressUs = 3LL * 1000000LL;
constexpr int64_t kWakeWordDebounceUs = 2LL * 1000000LL;
constexpr int64_t kWakeWordCooldownAfterCloseUs = 2LL * 1000000LL;
constexpr int64_t kListeningAutoStopGraceUs = 2500LL * 1000LL;
constexpr int64_t kListeningMaxDurationUs = 30LL * 1000000LL;

} // namespace


Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) {
    return state_machine_.TransitionTo(state);
}

bool Application::IsWifiResetConfirmationPending() {
    if (wifi_reset_confirmation_pending_ && esp_timer_get_time() > wifi_reset_confirmation_deadline_us_) {
        wifi_reset_confirmation_pending_ = false;
        wifi_reset_confirmation_deadline_us_ = 0;
        ESP_LOGW(TAG, "WiFi reset confirmation timeout");
    }
    return wifi_reset_confirmation_pending_;
}

void Application::ArmWakeWordCooldown(int64_t duration_us) {
    int64_t deadline_us = esp_timer_get_time() + duration_us;
    if (deadline_us > wake_word_ignore_until_us_) {
        wake_word_ignore_until_us_ = deadline_us;
    }
}

bool Application::IsListeningGracePeriodActive() const {
    if (listening_started_at_us_ <= 0) {
        return false;
    }
    int64_t now_us = esp_timer_get_time();
    return (now_us - listening_started_at_us_) < kListeningAutoStopGraceUs;
}

bool Application::ShouldIgnoreWakeWord(const std::string& wake_word) {
    int64_t now_us = esp_timer_get_time();
    if (now_us < wake_word_ignore_until_us_) {
        long remaining_ms = static_cast<long>((wake_word_ignore_until_us_ - now_us) / 1000);
        ESP_LOGI(TAG, "Ignore wake word \"%s\" during cooldown (%ld ms left)", wake_word.c_str(), remaining_ms);
        return true;
    }

    if (last_wake_word_accepted_us_ > 0 && now_us - last_wake_word_accepted_us_ < kWakeWordDebounceUs) {
        long delta_ms = static_cast<long>((now_us - last_wake_word_accepted_us_) / 1000);
        ESP_LOGI(TAG, "Ignore wake word \"%s\" (debounce %ld ms)", wake_word.c_str(), delta_ms);
        return true;
    }

    last_wake_word_accepted_us_ = now_us;
    return false;
}

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Setup the display
    auto display = board.GetDisplay();

    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    // Apply local assets early so wake word models are available before activation.
    auto& assets = Assets::GetInstance();
    if (assets.partition_valid()) {
        assets.Apply();
    }

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        ESP_LOGI(TAG, "VAD state: %s", speaking ? "speech" : "silence");
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);
    ArmWakeWordCooldown(kWakeWordStartupSuppressUs);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();
        
        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                display->SetChatMessage("system", Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    const EventBits_t ALL_EVENTS = 
        MAIN_EVENT_SCHEDULE |
        MAIN_EVENT_SEND_AUDIO |
        MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE |
        MAIN_EVENT_CLOCK_TICK |
        MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED |
        MAIN_EVENT_NETWORK_DISCONNECTED |
        MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING |
        MAIN_EVENT_STOP_LISTENING |
        MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            static int64_t last_send_fail_log_us = 0;
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    int64_t now_us = esp_timer_get_time();
                    if (now_us - last_send_fail_log_us > 2000000) {
                        last_send_fail_log_us = now_us;
                        ESP_LOGW(TAG, "SendAudio failed (channel_open=%d)", protocol_->IsAudioChannelOpened());
                    }
                    // Drop remaining packets to prevent stalling OpusCodecTask
                    while (audio_service_.PopPacketFromSendQueue())
                        ;
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        } else if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
                if (listening_mode_ == kListeningModeAutoStop &&
                    !audio_service_.IsVoiceDetected() &&
                    !IsWifiResetConfirmationPending() &&
                    !IsListeningGracePeriodActive()) {
                    HandleStopListeningEvent();
                }
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();

            if (local_song_playing_ && audio_service_.IsIdle()) {
                local_song_playing_ = false;
                ESP_LOGI(TAG, "Local song playback completed");
            }

            if (GetDeviceState() == kDeviceStateIdle &&
                !youtube_song_requested_ &&
                !YouTubeAudioPoller::GetInstance().IsPlaying() &&
                !audio_service_.IsWakeWordRunning() &&
                audio_service_.IsIdle()) {
                audio_service_.EnableWakeWordDetection(true);
            }

            if (wifi_reset_confirmation_pending_ && !IsWifiResetConfirmationPending()) {
                if (GetDeviceState() == kDeviceStateListening &&
                    listening_mode_ == kListeningModeAutoStop &&
                    !audio_service_.IsVoiceDetected() &&
                    !IsListeningGracePeriodActive()) {
                    HandleStopListeningEvent();
                }
            }
            
            if (GetDeviceState() == kDeviceStateListening &&
                listening_mode_ == kListeningModeAutoStop &&
                !IsWifiResetConfirmationPending() &&
                listening_started_at_us_ > 0) {
                int64_t now_us = esp_timer_get_time();
                if (now_us - listening_started_at_us_ > kListeningMaxDurationUs) {
                    ESP_LOGW(TAG, "Listening timeout, closing conversation");
                    Board::GetInstance().OnListeningAutoStop();
                    ArmWakeWordCooldown(kWakeWordCooldownAfterCloseUs);
                    CloseAudioChannel();
                    SetDeviceState(kDeviceStateIdle);
                }
            }
        
            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
            }
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation.
        // Handle race: WiFi connects but timeout timer already entered config mode.
        if (state == kDeviceStateWifiConfiguring) {
            ESP_LOGI(TAG, "WiFi connected during config mode, switching to activation");
        }
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate([](void* arg) {
            Application* app = static_cast<Application*>(arg);
            app->ActivationTask();
            app->activation_task_handle_ = nullptr;
            vTaskDelete(NULL);
        }, "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        if (protocol_) {
            protocol_->CloseAudioChannel();
        }
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    has_server_time_ = ota_->HasServerTime();

    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

    // Play the success sound to indicate the device is ready
    audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);

    // Release OTA object after activation is complete
    ota_.reset();

    // WakeNet has a relatively large one-time heap allocation on ESP32-C3.
    // Let the ready sound release its decode/resample buffers before entering
    // Idle, whose state handler initializes WakeNet.
    if (!audio_service_.WaitForPlaybackIdle(5000)) {
        ESP_LOGW(TAG, "Timed out waiting for activation sound playback to drain");
    }

    SetDeviceState(kDeviceStateIdle);

    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    // Start YouTube audio poller to check for pending song commands
    YouTubeAudioPoller::GetInstance().Start();
}

void Application::ActivationTask() {
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version
    CheckNewVersion();

    // Initialize the protocol
    InitializeProtocol();

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [display](int progress, size_t speed) -> void {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            display->SetChatMessage("system", buffer);
        });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 4;
    const int MAX_RETRY_DELAY = 30;
    int retry_count = 0;
    int retry_delay = 5; // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries (%d), skip version check", MAX_RETRY);
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay = std::min(retry_delay * 2, MAX_RETRY_DELAY);
            continue;
        }
        retry_count = 0;
        retry_delay = 5; // Reset retry delay

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }

        // Make sure we really start recording immediately after the channel is up.
        // Without this, when wake-word sets the state to Listening before the
        // socket finishes opening, the cloud never receives StartListening and
        // will ignore subsequent audio packets.
        if (GetDeviceState() == kDeviceStateListening) {
            ESP_LOGI(TAG, "Audio channel opened while already in listening state, sending StartListening");
            if (protocol_) {
                protocol_->SendStartListening(listening_mode_);
            }
            if (!audio_service_.IsAudioProcessorRunning()) {
                audio_service_.EnableVoiceProcessing(true);
            }
        }
    });
    
    protocol_->OnAudioChannelClosed([this, &board]() {
        if (!YouTubeAudioPoller::GetInstance().IsPlaying()) {
            board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        }
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            ArmWakeWordCooldown(kWakeWordCooldownAfterCloseUs);
            if (!YouTubeAudioPoller::GetInstance().IsPlaying()) {
                SetDeviceState(kDeviceStateIdle);
            }
        });
    });
    
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        if (!cJSON_IsObject(root)) {
            ESP_LOGW(TAG, "Ignore invalid incoming JSON payload");
            return;
        }

        auto type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type) || type->valuestring == nullptr) {
            ESP_LOGW(TAG, "Ignore incoming JSON without valid type");
            return;
        }

        const char* type_value = type->valuestring;
        if (strcmp(type_value, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (!cJSON_IsString(state) || state->valuestring == nullptr) {
                ESP_LOGW(TAG, "Ignore TTS message without valid state");
                return;
            }

            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    if (IsWifiResetConfirmationPending()) {
                        ESP_LOGI(TAG, "Ignore TTS start while waiting for WiFi reset confirmation");
                        if (protocol_) {
                            protocol_->SendAbortSpeaking(kAbortReasonWakeWordDetected);
                            protocol_->SendStartListening(kListeningModeAutoStop);
                        }
                        if (GetDeviceState() != kDeviceStateListening) {
                            SetDeviceState(kDeviceStateListening);
                        }
                        return;
                    }
                    aborted_ = false;
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (youtube_song_requested_ || YouTubeAudioPoller::GetInstance().IsPlaying()) {
                        bool was_requested = youtube_song_requested_;
                        std::string q = std::move(youtube_song_query_);
                        youtube_song_query_.clear();
                        if (!q.empty()) {
                            YouTubeAudioPoller::GetInstance().PlayQuery(q);
                        } else if (was_requested) {
                            YouTubeAudioPoller::GetInstance().TriggerFetch();
                        }
                        youtube_song_requested_ = false;
                        if (protocol_ && protocol_->IsAudioChannelOpened()) {
                            protocol_->CloseAudioChannel();
                        }
                        SetDeviceState(kDeviceStateIdle);
                    } else if (GetDeviceState() == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            CloseAudioChannel();
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    if (IsWifiResetConfirmationPending()) {
                        ESP_LOGI(TAG, "Ignore TTS sentence while waiting for WiFi reset confirmation");
                        return;
                    }
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([this, display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type_value, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([this, display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                    if (TryHandleModeSwitchCommand(message)) {
                        return;
                    }
                    if (TryHandleWifiResetCommand(message)) {
                        return;
                    }
                    if (TryHandleLocalSongCommand(message)) {
                        return;
                    }
                    static const std::vector<std::string> play_prefixes = {
                        "tolong putarkan lagu ", "tolong putar lagu ",
                        "tolong putarkan musik ", "tolong putar musik ",
                        "tolong putarkan ", "tolong putar ",
                        "putarkan lagu ", "putar lagu ",
                        "putarkan musik ", "putar musik ",
                        "play song ", "play music "
                    };
                    std::string lower_msg = ToLowerAscii(message);
                    std::string found_query;
                    for (const auto& p : play_prefixes) {
                        auto pos = lower_msg.find(p);
                        if (pos != std::string::npos && (pos == 0 || pos <= 15)) {
                            std::string raw_q = message.substr(pos + p.length());
                            while (!raw_q.empty() && (raw_q.front() == ' ' || raw_q.front() == ':' || raw_q.front() == ',')) raw_q.erase(0, 1);
                            while (!raw_q.empty() && (raw_q.back() == ' ' || raw_q.back() == '.' || raw_q.back() == '?' || raw_q.back() == '!')) raw_q.pop_back();
                            if (!raw_q.empty()) {
                                found_query = raw_q;
                                break;
                            }
                        }
                    }
                    if (!found_query.empty()) {
                        youtube_song_requested_ = true;
                        youtube_song_query_ = found_query;
                        ESP_LOGI(TAG, "Extracted YouTube song query from voice command: \"%s\"", youtube_song_query_.c_str());
                    }
                    TryHandleStandbyCommand(message);
                });
            }
        } else if (strcmp(type_value, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type_value, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type_value, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type_value, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
        } else if (strcmp(type_value, "play_audio") == 0) {
            auto vid_item = cJSON_GetObjectItem(root, "video_id");
            auto title_item = cJSON_GetObjectItem(root, "title");
            if (cJSON_IsString(vid_item)) {
                std::string vid = vid_item->valuestring;
                std::string tit = cJSON_IsString(title_item) ? title_item->valuestring : "YouTube Song";
                ESP_LOGI(TAG, "Direct play_audio message received: title=\"%s\", vid=%s", tit.c_str(), vid.c_str());
                YouTubeAudioPoller::GetInstance().PlaySong(vid, tit);
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type_value, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            char* root_json = cJSON_PrintUnformatted(root);
            ESP_LOGI(TAG, "Received custom message: %s", root_json ? root_json : "<oom>");
            if (root_json) {
                cJSON_free(root_json);
            }
            if (cJSON_IsObject(payload)) {
                std::string payload_str("{}");
                char* payload_json = cJSON_PrintUnformatted(payload);
                if (payload_json) {
                    payload_str.assign(payload_json);
                    cJSON_free(payload_json);
                }
                Schedule([this, display, payload_str = std::move(payload_str)]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type_value);
        }
    });
    
    protocol_->Start();
}

bool Application::PlayOggFile(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return false;
    }

    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        return false;
    }
    long file_size = std::ftell(f);
    if (file_size <= 0) {
        std::fclose(f);
        return false;
    }
    std::rewind(f);

    std::vector<char> buffer(static_cast<size_t>(file_size));
    size_t bytes_read = std::fread(buffer.data(), 1, buffer.size(), f);
    std::fclose(f);
    if (bytes_read != buffer.size()) {
        return false;
    }

    audio_service_.PlaySound(std::string_view(buffer.data(), buffer.size()));
    return true;
}

bool Application::StopLocalSongPlayback(bool show_notification) {
    if (!local_song_playing_) {
        return false;
    }

    ESP_LOGI(TAG, "Stopping local song playback");
    audio_service_.ResetDecoder();
    local_song_playing_ = false;

    if (show_notification) {
        auto display = Board::GetInstance().GetDisplay();
        display->ShowNotification("Song stopped");
    }
    return true;
}

bool Application::PlayLocalSong(int song_index) {
    std::array<std::string, 5> candidates;
    switch (song_index) {
        case 2:
            candidates = {
                "/spiffs/song2.ogg",
                "/spiffs/songs/song2.ogg",
                "song2.ogg",
                "songs/song2.ogg",
                "music/song2.ogg"
            };
            break;
        case 3:
            candidates = {
                "/spiffs/song3.ogg",
                "/spiffs/songs/song3.ogg",
                "song3.ogg",
                "songs/song3.ogg",
                "music/song3.ogg"
            };
            break;
        case 1:
        default:
            candidates = {
                "/spiffs/song1.ogg",
                "/spiffs/songs/song1.ogg",
                "song1.ogg",
                "songs/song1.ogg",
                "music/song1.ogg"
            };
            break;
    }

    auto& assets = Assets::GetInstance();
    for (const auto& candidate : candidates) {
        if (candidate.empty()) {
            continue;
        }

        if (PlayOggFile(candidate)) {
            ESP_LOGI(TAG, "Playing local song file: %s", candidate.c_str());
            local_song_playing_ = true;
            return true;
        }

        void* ptr = nullptr;
        size_t size = 0;
        if (assets.GetAssetData(candidate, ptr, size)) {
            ESP_LOGI(TAG, "Playing local song asset: %s", candidate.c_str());
            audio_service_.PlaySound(std::string_view(static_cast<const char*>(ptr), size));
            local_song_playing_ = true;
            return true;
        }
    }

    return false;
}

bool Application::ClearStoredWifiCredentials() {
    nvs_handle_t nvs_handle = 0;
    esp_err_t ret = nvs_open("wifi", NVS_READWRITE, &nvs_handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "WiFi namespace not found, nothing to clear");
        return true;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open WiFi namespace: %s", esp_err_to_name(ret));
        return false;
    }

    ret = nvs_erase_all(nvs_handle);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to clear WiFi credentials: %s", esp_err_to_name(ret));
        return false;
    }

    ESP_LOGI(TAG, "Stored WiFi credentials cleared");
    return true;
}

bool Application::TryHandleWifiResetCommand(const std::string& text) {
    std::string lowered_text = ToLowerAscii(text);
    std::string normalized_text = NormalizeCommandText(lowered_text);
    int64_t now_us = esp_timer_get_time();
    auto display = Board::GetInstance().GetDisplay();

    if (IsWifiResetConfirmationPending()) {
        if (IsWifiResetCancelCommand(lowered_text)) {
            wifi_reset_confirmation_pending_ = false;
            wifi_reset_confirmation_deadline_us_ = 0;
            display->ShowNotification("WiFi reset cancelled");
            return true;
        }

        bool is_confirm = IsWifiResetConfirmCommand(lowered_text) ||
                          IsWifiResetQuickConfirmCommand(normalized_text);
        if (!is_confirm) {
            return false;
        }

        wifi_reset_confirmation_pending_ = false;
        wifi_reset_confirmation_deadline_us_ = 0;

        if (!ClearStoredWifiCredentials()) {
            display->ShowNotification("Failed to reset SSID");
            audio_service_.PlaySound(Lang::Sounds::OGG_EXCLAMATION);
            return true;
        }

        display->ShowNotification("SSID reset, rebooting...");
        Reboot();
        return true;
    }

    if (IsWifiResetConfirmCommand(lowered_text) || IsWifiResetQuickConfirmCommand(normalized_text)) {
        display->ShowNotification("Say 'reset wifi' first");
        return true;
    }

    if (!IsWifiResetCommand(lowered_text)) {
        return false;
    }

    wifi_reset_confirmation_pending_ = true;
    wifi_reset_confirmation_deadline_us_ = now_us + kWifiResetConfirmWindowUs;
    display->ShowNotification("Say: ya / konfirmasi reset wifi (15s)");
    audio_service_.PlaySound(Lang::Sounds::OGG_EXCLAMATION);
    if (GetDeviceState() == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Return to listening for WiFi reset confirmation");
        if (protocol_) {
            protocol_->SendAbortSpeaking(kAbortReasonWakeWordDetected);
            protocol_->SendStartListening(kListeningModeAutoStop);
        }
        SetDeviceState(kDeviceStateListening);
    } else if (protocol_ && GetDeviceState() == kDeviceStateListening) {
        protocol_->SendStartListening(kListeningModeAutoStop);
    }
    ESP_LOGW(TAG, "WiFi reset command detected, waiting for confirmation");
    return true;
}

bool Application::TryHandleModeSwitchCommand(const std::string& text) {
    const std::string normalized_text = NormalizeCommandText(ToLowerAscii(text));
    if (!IsModeSwitchCommand(normalized_text)) {
        return false;
    }

    // Determine target mode from voice command
    BootMode target_mode = BootMode::Chronchi;  // default "pindah mode" = Chronchi
    const char* mode_label = "Chronchi";

    if (ContainsNormalizedPhrase(normalized_text, "xiaozhi") ||
               ContainsNormalizedPhrase(normalized_text, "awal") ||
               ContainsNormalizedPhrase(normalized_text, "normal")) {
        target_mode = BootMode::Xiaozhi;
        mode_label = "Xiaozhi";
    }

    // If already in the target mode, just notify
    if (ModeStore::Current() == target_mode) {
        ESP_LOGI(TAG, "Already in %s mode", mode_label);
        auto display = Board::GetInstance().GetDisplay();
        char msg[64];
        snprintf(msg, sizeof(msg), "Sudah di mode %s", mode_label);
        display->ShowNotification(msg);
        return true;
    }

    ESP_LOGW(TAG, "Voice mode-switch command detected: \"%s\" -> %s", text.c_str(), mode_label);
    const esp_err_t err = ModeStore::Save(target_mode);
    auto display = Board::GetInstance().GetDisplay();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save %s mode: %s", mode_label, esp_err_to_name(err));
        char msg[64];
        snprintf(msg, sizeof(msg), "Gagal pindah ke %s", mode_label);
        display->ShowNotification(msg);
        audio_service_.PlaySound(Lang::Sounds::OGG_EXCLAMATION);
        return true;
    }

    wifi_reset_confirmation_pending_ = false;
    wifi_reset_confirmation_deadline_us_ = 0;
    char msg[64];
    snprintf(msg, sizeof(msg), "Pindah ke %s...", mode_label);
    display->ShowNotification(msg);
    Reboot();
    return true;
}

bool Application::TryHandleLocalSongCommand(const std::string& text) {
    std::string lowered_text = ToLowerAscii(text);
    if ((local_song_playing_ || youtube_song_requested_ ||
         YouTubeAudioPoller::GetInstance().IsPlaying()) &&
        IsLocalSongStopCommand(lowered_text)) {
        ESP_LOGI(TAG, "Song stop command detected: \"%s\"", text.c_str());

        // Clear any queued voice request before returning to Idle.
        youtube_song_requested_ = false;
        youtube_song_query_.clear();

        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        StopLocalSongPlayback(true);
        YouTubeAudioPoller::GetInstance().Abort();
        SetDeviceState(kDeviceStateIdle);
        return true;
    }

    int song_index = ParseLocalSongIndex(lowered_text);
    if (song_index == 0) {
        return false;
    }

    ESP_LOGI(TAG, "Local song command detected: \"%s\" -> song %d", text.c_str(), song_index);

    if (GetDeviceState() == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    SetDeviceState(kDeviceStateIdle);
    StopLocalSongPlayback(false);

    if (PlayLocalSong(song_index)) {
        auto display = Board::GetInstance().GetDisplay();
        if (song_index == 2) {
            display->ShowNotification("Playing song 2");
        } else if (song_index == 3) {
            display->ShowNotification("Playing song 3");
        } else {
            display->ShowNotification("Playing song 1");
        }
        return true;
    }

    auto display = Board::GetInstance().GetDisplay();
    if (song_index == 2) {
        display->ShowNotification("song2.ogg not found");
    } else if (song_index == 3) {
        display->ShowNotification("song3.ogg not found");
    } else {
        display->ShowNotification("Gunakan YouTube stream");
    }
    audio_service_.PlaySound(Lang::Sounds::OGG_EXCLAMATION);
    return true;
}

bool Application::TryHandleStandbyCommand(const std::string& text) {
    if (GetDeviceState() != kDeviceStateListening) {
        return false;
    }

    std::string normalized_text = NormalizeCommandText(ToLowerAscii(text));
    if (!IsStandbyCommand(normalized_text)) {
        return false;
    }

    ESP_LOGI(TAG, "Standby command detected: \"%s\"", text.c_str());
    wifi_reset_confirmation_pending_ = false;
    wifi_reset_confirmation_deadline_us_ = 0;

    auto display = Board::GetInstance().GetDisplay();
    display->ShowNotification("Standby");

    // Close channel immediately and arm cooldown to prevent wake word
    // from detecting server's goodbye TTS as a new trigger.
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->SendStopListening();
        protocol_->CloseAudioChannel();
    }
    ArmWakeWordCooldown(kWakeWordCooldownAfterCloseUs);
    standby_until_us_ = esp_timer_get_time() + 60LL * 1000000LL;  // Block hands-free 60s
    SetDeviceState(kDeviceStateIdle);
    return true;
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
#if CONFIG_IDF_TARGET_ESP32C3
        bool was_wake_word = audio_service_.IsWakeWordRunning();
        if (was_wake_word) {
            audio_service_.EnableWakeWordDetection(false);
        }
        audio_service_.PlaySound(sound);
#else
        audio_service_.PlaySound(sound);
#endif
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT);
}

void Application::StartListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
}

void Application::StopListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

void Application::HandleToggleChatEvent() {
    // Cancel active playback or a song request that has not started yet.
    if (youtube_song_requested_ || YouTubeAudioPoller::GetInstance().IsPlaying()) {
        ESP_LOGI(TAG, "Button clicked: stopping/cancelling YouTube audio");
        youtube_song_requested_ = false;
        youtube_song_query_.clear();
        YouTubeAudioPoller::GetInstance().Abort();

        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }

        auto display = Board::GetInstance().GetDisplay();
        if (display != nullptr) {
            display->ShowNotification("Lagu dihentikan");
        }
        return;
    }

    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateConnecting) {
        // User clicked button while connecting - abort and reset to idle!
        ESP_LOGW(TAG, "User clicked button while connecting, aborting to idle");
        if (protocol_) {
            protocol_->CloseAudioChannel();
        }
        SetDeviceState(kDeviceStateIdle);
        auto display = Board::GetInstance().GetDisplay();
        if (display != nullptr) {
            display->ShowNotification("Dibatalkan");
        }
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        audio_service_.EnableWakeWordDetection(false);
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                ESP_LOGE(TAG, "Failed to open audio channel, reverting to idle");
                SetDeviceState(kDeviceStateIdle);
                auto display = Board::GetInstance().GetDisplay();
                if (display != nullptr) {
                    display->ShowNotification("Koneksi gagal");
                }
                return;
            }
        }

        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        ArmWakeWordCooldown(kWakeWordCooldownAfterCloseUs);
        protocol_->CloseAudioChannel();
    }
}

void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    // Push-to-talk can always interrupt local song playback.
    StopLocalSongPlayback(false);

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                SetDeviceState(kDeviceStateIdle);
                return;
            }
        }

        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateIdle) {
        if (StopLocalSongPlayback(true)) {
            return;
        }
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
        }
        audio_service_.EnableVoiceProcessing(false);
        auto display = Board::GetInstance().GetDisplay();
        if (display != nullptr) {
            display->SetStatus(Lang::Strings::PLEASE_WAIT);
        }
    }
}

void Application::HandleWakeWordDetectedEvent() {
    if (!protocol_) {
        return;
    }

    if (YouTubeAudioPoller::GetInstance().IsPlaying()) {
        ESP_LOGW(TAG, "Wake word ignored: YouTube audio is currently playing");
        return;
    }

    // Clear standby flag — user explicitly woke the device
    standby_until_us_ = 0;

    auto state = GetDeviceState();

    if (state == kDeviceStateIdle || state == kDeviceStateConnecting) {
        auto wake_word = audio_service_.GetLastWakeWord();
        if (ShouldIgnoreWakeWord(wake_word)) {
            return;
        }
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            // If we were already in "connecting", re-run open to guarantee a fresh channel.
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                SetDeviceState(kDeviceStateIdle);
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
        if (TryHandleWifiResetCommand(wake_word)) {
            if (IsWifiResetConfirmationPending()) {
                SetListeningMode(kListeningModeAutoStop);
            }
            return;
        }
#if CONFIG_SEND_WAKE_WORD_DATA
        // Encode and send the wake word data to the server
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
        }
        // Set the chat state to wake word detected
        protocol_->SendWakeWordDetected(wake_word);
        audio_service_.EnableVoiceProcessing(true);
        audio_service_.EnableWakeWordDetection(false);
        // Otomatis berhenti setelah 30 detik tanpa suara
        SetListeningMode(kListeningModeAutoStop);
#else
        // Set flag to play popup sound after state changes to listening
        // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
        play_popup_on_listening_ = true;
        audio_service_.EnableVoiceProcessing(true);
        audio_service_.EnableWakeWordDetection(false);
        SetListeningMode(kListeningModeAutoStop);
#endif
        // Past this point kita sudah niat listening; pastikan server tahu.
        if (protocol_->IsAudioChannelOpened()) {
            protocol_->SendStartListening(listening_mode_);
        }
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    
    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            listening_started_at_us_ = 0;
            audio_service_.EnableVoiceProcessing(false);
            if (YouTubeAudioPoller::GetInstance().IsPlaying() || youtube_song_requested_) {
                board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
                display->SetStatus("Memutar Lagu");
                display->SetEmotion("music");
                display->SetFaceState(FaceState::Music);
                audio_service_.EnableWakeWordDetection(false);
            } else {
                board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
                display->SetStatus(Lang::Strings::STANDBY);
                display->SetEmotion("neutral");
                display->SetFaceState(FaceState::Idle);
                audio_service_.EnableWakeWordDetection(true);
            }
            break;
        case kDeviceStateConnecting:
            board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
            listening_started_at_us_ = 0;
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetFaceState(FaceState::Idle);
            display->SetChatMessage("system", "");
            audio_service_.EnableWakeWordDetection(false);
            break;
        case kDeviceStateListening:
            board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
            listening_started_at_us_ = esp_timer_get_time();
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");
            display->SetFaceState(FaceState::Listening);

            audio_service_.EnableWakeWordDetection(false);
            if (!audio_service_.IsAudioProcessorRunning()) {
                audio_service_.EnableVoiceProcessing(true);
            }

            if (protocol_ && protocol_->IsAudioChannelOpened()) {
                protocol_->SendStartListening(listening_mode_);
            }

            // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
            if (play_popup_on_listening_) {
                play_popup_on_listening_ = false;
                audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            }
            break;
        case kDeviceStateSpeaking:
            board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
            listening_started_at_us_ = 0;
            display->SetStatus(Lang::Strings::SPEAKING);
            display->SetFaceState(FaceState::Speaking);

            // Pastikan speaker aktif ketika mulai berbicara
            board.GetAudioCodec()->EnableOutput(true);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
            break;
        case kDeviceStateWifiConfiguring:
            listening_started_at_us_ = 0;
            display->SetFaceState(FaceState::Idle);
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            listening_started_at_us_ = 0;
            // Do nothing
            break;
    }
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    youtube_song_requested_ = false;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::CloseAudioChannel() {
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    listening_started_at_us_ = esp_timer_get_time();
    SetDeviceState(kDeviceStateListening);
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        display->SetChatMessage("system", buffer);
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start(); // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER); // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    if (YouTubeAudioPoller::GetInstance().IsPlaying()) {
        ESP_LOGW(TAG, "Wake word invoke ignored: YouTube audio is currently playing");
        return;
    }

    auto state = GetDeviceState();
    
    if (state == kDeviceStateIdle) {
        if (ShouldIgnoreWakeWord(wake_word)) {
            return;
        }
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
        if (TryHandleWifiResetCommand(wake_word)) {
            if (IsWifiResetConfirmationPending()) {
                SetListeningMode(kListeningModeAutoStop);
            }
            return;
        }
#if CONFIG_USE_AFE_WAKE_WORD || CONFIG_USE_CUSTOM_WAKE_WORD
        // Jika butuh data wake word, tetap buffer lokal saja (tidak dikirim server)
        audio_service_.PopWakeWordPacket();
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
        // Set flag to play popup sound after state changes to listening
        // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
        play_popup_on_listening_ = true;
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#endif
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (state == kDeviceStateListening) {
        Schedule([this]() {
            if (protocol_) {
                protocol_->SendStopListening();
                protocol_->CloseAudioChannel();
            }
            ArmWakeWordCooldown(kWakeWordCooldownAfterCloseUs);
            SetDeviceState(kDeviceStateIdle);
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (YouTubeAudioPoller::GetInstance().IsPlaying()) {
        return false;
    }

    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload = std::move(payload)]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::ResetProtocol() {
    Schedule([this]() {
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}
