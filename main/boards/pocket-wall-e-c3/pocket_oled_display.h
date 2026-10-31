#ifndef POCKET_OLED_DISPLAY_H
#define POCKET_OLED_DISPLAY_H

#include "display/oled_display.h"

#include <lvgl.h>

class PocketOledDisplay : public OledDisplay {
public:
    PocketOledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                      int height, bool mirror_x, bool mirror_y);
    ~PocketOledDisplay() override;

    void SetupUI() override;
    // Status khusus untuk tampilan radio (tanpa mengubah kelas Display dasar):
    //   "RADIO|<nama stasiun>|<frekuensi>"  -> tampilkan layar radio
    //   "RADIO_END"                          -> kembali ke wajah Wall-E
    void SetStatus(const char* status) override;
    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void ClearChatMessages() override;

private:
    enum class FaceMode {
        kNeutral,
        kListening,
        kThinking,
        kSpeaking,
        kHappy,
        kSad,
        kAngry,
        kSleepy,
    };

    static constexpr int kRadioBarCount = 5;

    lv_obj_t* face_layer_ = nullptr;
    lv_obj_t* left_eye_ = nullptr;
    lv_obj_t* right_eye_ = nullptr;
    lv_obj_t* left_top_lid_ = nullptr;
    lv_obj_t* right_top_lid_ = nullptr;
    lv_obj_t* left_bottom_lid_ = nullptr;
    lv_obj_t* right_bottom_lid_ = nullptr;
    lv_obj_t* chat_line_ = nullptr;
    lv_timer_t* animation_timer_ = nullptr;

    // Layar radio.
    lv_obj_t* radio_layer_ = nullptr;
    lv_obj_t* radio_name_ = nullptr;
    lv_obj_t* radio_freq_ = nullptr;
    lv_obj_t* radio_fm_ = nullptr;
    lv_obj_t* radio_needle_ = nullptr;
    lv_obj_t* radio_bars_[kRadioBarCount] = {};
    int radio_bar_height_[kRadioBarCount] = {};
    bool radio_active_ = false;

    FaceMode face_mode_ = FaceMode::kNeutral;
    uint32_t animation_tick_ = 0;
    uint32_t random_state_ = 0x57414C4C;
    uint32_t next_blink_tick_ = 45;
    uint32_t blink_until_tick_ = 0;
    uint32_t next_idle_tick_ = 30;
    int current_look_x_ = 0;
    int current_look_y_ = 0;
    int target_look_x_ = 0;
    int target_look_y_ = 0;
    int current_eye_height_ = 2;
    bool has_chat_text_ = false;

    static void AnimationTimerCallback(lv_timer_t* timer);
    void UpdateAnimationFrame();
    void ApplyLayout();
    void SetFaceMode(FaceMode mode);
    FaceMode FaceModeFromEmotion(const char* emotion) const;
    uint32_t NextRandom();

    void BuildRadioUI(lv_obj_t* screen);
    void ShowRadio(const char* payload);
    void HideRadio();
    void UpdateRadioFrame();
};

#endif  // POCKET_OLED_DISPLAY_H
