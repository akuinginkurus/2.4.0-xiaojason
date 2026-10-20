#ifndef YOUTUBE_AUDIO_POLLER_H_
#define YOUTUBE_AUDIO_POLLER_H_

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string>
#include <atomic>
#include <mutex>

class Http;

class YouTubeAudioPoller {
public:
    static YouTubeAudioPoller& GetInstance();

    void Start();
    static std::string GetServerDomain();
    static std::string GetBaseUrl();
    static std::string GetStreamUrl();
    void Stop();
    bool IsPlaying() const {
        return is_playing_.load() || play_requested_.load() || query_requested_.load() ||
               trigger_requested_.load() || is_fetching_.load();
    }
    std::string GetCurrentTitle() const;
    std::string GetCurrentVideoId() const;
    void Abort();
    void TriggerFetch();
    void PlaySong(const std::string& video_id, const std::string& title);
    void PlayQuery(const std::string& query);

private:
    YouTubeAudioPoller();
    \~YouTubeAudioPoller();

    void TaskLoop();
    bool FetchDirect(const std::string& query, std::string& video_id, std::string& title);
    bool FetchCommands(int& cmd_id, std::string& video_id, std::string& title);
    bool AckCommand(int cmd_id);
    bool ReportStatus(const std::string& status, const std::string& video_id = "");
    void StreamAudio(const std::string& video_id, const std::string& title);

    TaskHandle_t task_handle_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> is_playing_{false};
    std::atomic<bool> abort_requested_{false};
    std::atomic<bool> trigger_requested_{false};
    std::atomic<bool> play_requested_{false};
    std::atomic<bool> query_requested_{false};
    std::atomic<bool> is_fetching_{false};
    std::mutex song_mutex_;
    std::string pending_video_id_;
    std::string pending_title_;
    std::string pending_query_;
    std::string current_video_id_;
    std::string current_title_;

    std::mutex http_mutex_;
    Http* active_http_ = nullptr;
};

#endif  // YOUTUBE_AUDIO_POLLER_H_
