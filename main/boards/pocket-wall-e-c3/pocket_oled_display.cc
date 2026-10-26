#include "pocket_oled_display.h"

#include "assets/lang_config.h"

#include <algorithm>
#include <cstring>

namespace {

constexpr uint32_t kAnimationPeriodMs = 40;
constexpr uint32_t kMusicUpdatePeriodMs = 75;

void PrepareShape(lv_obj_t* object) {
    lv_obj_remove_style_all(object);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

bool Contains(const char* text, const char* value) {
    return text != nullptr &&
           value != nullptr &&
           std::strstr(text, value) != nullptr;
}

int Ease(int current, int target) {
    if (current == target) {
        return current;
    }

    const int delta = target - current;

    if (delta > 0) {
        return current + std::max(1, delta / 3);
    }

    return current + std::min(-1, delta / 3);
}

}  // namespace

PocketOledDisplay::PocketOledDisplay(
    esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_handle_t panel,
    int width,
    int height,
    bool mirror_x,
    bool mirror_y)
    : OledDisplay(
          panel_io, panel, width, height, mirror_x, mirror_y) {
}

PocketOledDisplay::~PocketOledDisplay() {
    if (animation_timer_ != nullptr) {
        lv_timer_delete(animation_timer_);
        animation_timer_ = nullptr;
    }

    if (chat_line_ != nullptr) {
        lv_obj_delete(chat_line_);
        chat_line_ = nullptr;
    }

    if (face_layer_ != nullptr) {
        lv_obj_delete(face_layer_);
        face_layer_ = nullptr;
    }
}

void PocketOledDisplay::SetupUI() {
    OledDisplay::SetupUI();

    DisplayLockGuard lock(this);

    auto* screen = lv_screen_active();

    // Face area: only the top 48 pixels of the OLED.
    face_layer_ = lv_obj_create(screen);
    PrepareShape(face_layer_);

    lv_obj_set_style_bg_color(
        face_layer_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(
        face_layer_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(
        face_layer_, 0, 0);
    lv_obj_set_style_radius(
        face_layer_, 0, 0);
    lv_obj_set_style_pad_all(
        face_layer_, 0, 0);

    left_eye_ = lv_obj_create(face_layer_);
    right_eye_ = lv_obj_create(face_layer_);

    left_top_lid_ = lv_obj_create(face_layer_);
    right_top_lid_ = lv_obj_create(face_layer_);

    left_bottom_lid_ = lv_obj_create(face_layer_);
    right_bottom_lid_ = lv_obj_create(face_layer_);

    // Normal eyes.
    for (auto* eye : {left_eye_, right_eye_}) {
        PrepareShape(eye);

        lv_obj_set_style_bg_color(
            eye, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(
            eye, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(
            eye, 8, 0);
    }

    // Create the nine equalizer bars.
    // They remain hidden until music playback begins.
    for (int i = 0; i < kMusicBarCount; ++i) {
        music_bars_[i] = lv_obj_create(face_layer_);

        PrepareShape(music_bars_[i]);

        lv_obj_set_style_bg_color(
            music_bars_[i], lv_color_white(), 0);
        lv_obj_set_style_bg_opa(
            music_bars_[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(
            music_bars_[i], 2, 0);

        lv_obj_add_flag(
            music_bars_[i], LV_OBJ_FLAG_HIDDEN);

        music_heights_[i] = 5;
        music_targets_[i] = 5;
    }

    // Eyelids used by the normal face animations.
    for (auto* lid : {
             left_top_lid_,
             right_top_lid_,
             left_bottom_lid_,
             right_bottom_lid_}) {
        PrepareShape(lid);

        lv_obj_set_style_bg_color(
            lid, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(
            lid, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(
            lid, 6, 0);

        lv_obj_add_flag(
            lid, LV_OBJ_FLAG_HIDDEN);
    }

    // Bottom line: intentionally unchanged.
    // Existing status and song-duration text remain here.
    chat_line_ = lv_label_create(screen);

    lv_obj_set_pos(chat_line_, 0, 48);
    lv_obj_set_size(chat_line_, 128, 16);

    lv_obj_set_style_bg_color(
        chat_line_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(
        chat_line_, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(
        chat_line_, lv_color_white(), 0);
    lv_obj_set_style_text_align(
        chat_line_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_left(
        chat_line_, 2, 0);
    lv_obj_set_style_pad_right(
        chat_line_, 2, 0);

    lv_label_set_long_mode(
        chat_line_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(chat_line_, "");

    ApplyLayout();

    lv_obj_move_foreground(chat_line_);

    animation_timer_ = lv_timer_create(
        AnimationTimerCallback,
        kAnimationPeriodMs,
        this);
}

void PocketOledDisplay::SetStatus(const char* status) {
    OledDisplay::SetStatus(status);

    DisplayLockGuard lock(this);

    if (status == nullptr) {
        return;
    }

    const bool is_standby =
        std::strcmp(status, Lang::Strings::STANDBY) == 0;

    // Remember the current status.
    // This prevents SetEmotion("neutral") from undoing sleepy eyes
    // when Application enters standby.
    standby_mode_ = is_standby;

    // Enter music mode when the existing player reports playback.
    if (Contains(status, "Memutar lagu") ||
        Contains(status, "memutar lagu")) {
        SetMusicMode(true);
    }
    // Leave music mode when normal listening resumes or playback
    // returns to standby.
    else if (Contains(status, "Mendengarkan") ||
             Contains(status, "mendengarkan") ||
             is_standby) {
        SetMusicMode(false);
    }

    if (music_mode_) {
        // Preserve the existing bottom-line status/duration behavior.
        if (!has_chat_text_ && chat_line_ != nullptr) {
            lv_label_set_text(chat_line_, status);
        }

        // Do not let ordinary face animations overwrite the spectrum.
        return;
    }

    if (std::strcmp(
            status, Lang::Strings::LISTENING) == 0) {
        SetFaceMode(FaceMode::kListening);
    } else if (std::strcmp(
                   status, Lang::Strings::SPEAKING) == 0) {
        SetFaceMode(FaceMode::kSpeaking);
    } else if (
        std::strcmp(status, Lang::Strings::CONNECTING) == 0 ||
        Contains(status, "Đang") ||
        Contains(status, "đang")) {
        SetFaceMode(FaceMode::kThinking);
    } else if (is_standby) {
        // Sleepy expression while waiting for the next interaction.
        SetFaceMode(FaceMode::kSleepy);
    }

    if (!has_chat_text_ && chat_line_ != nullptr) {
        lv_label_set_text(chat_line_, status);
    }
}

void PocketOledDisplay::SetEmotion(const char* emotion) {
    OledDisplay::SetEmotion(emotion);

    DisplayLockGuard lock(this);

    // Music playback keeps its own visualizer.
    if (music_mode_) {
        return;
    }

    // Application sends "neutral" immediately after setting STANDBY.
    // Keep the sleepy expression during standby instead of opening
    // the eyes again.
    if (standby_mode_ &&
        (emotion == nullptr ||
         std::strcmp(emotion, "neutral") == 0)) {
        SetFaceMode(FaceMode::kSleepy);
        return;
    }

    // Any other emotion follows the original emotion mapping.
    SetFaceMode(FaceModeFromEmotion(emotion));
}

void PocketOledDisplay::SetChatMessage(
    const char* role,
    const char* content) {
    DisplayLockGuard lock(this);

    has_chat_text_ =
        content != nullptr && content[0] != '\0';

    if (chat_line_ != nullptr) {
        lv_label_set_text(
            chat_line_,
            has_chat_text_ ? content : "");
    }

    if (has_chat_text_ &&
        role != nullptr &&
        std::strcmp(role, "assistant") == 0 &&
        face_mode_ == FaceMode::kNeutral &&
        !music_mode_) {
        SetFaceMode(FaceMode::kSpeaking);
    }
}

void PocketOledDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);

    has_chat_text_ = false;

    if (chat_line_ != nullptr) {
        lv_label_set_text(chat_line_, "");
    }
}

void PocketOledDisplay::AnimationTimerCallback(
    lv_timer_t* timer) {
    auto* display = static_cast<PocketOledDisplay*>(
        lv_timer_get_user_data(timer));

    if (display != nullptr) {
        display->UpdateAnimationFrame();
    }
}

void PocketOledDisplay::ApplyLayout() {
    if (face_layer_ == nullptr) {
        return;
    }

    lv_obj_set_pos(face_layer_, 0, 0);
    lv_obj_set_size(face_layer_, 128, 48);

    lv_obj_set_style_radius(left_eye_, 9, 0);
    lv_obj_set_style_radius(right_eye_, 9, 0);

    current_eye_height_ =
        std::min(current_eye_height_, 36);

    lv_obj_move_foreground(face_layer_);

    if (chat_line_ != nullptr) {
        lv_obj_move_foreground(chat_line_);
    }

    UpdateAnimationFrame();
}

void PocketOledDisplay::UpdateAnimationFrame() {
    if (face_layer_ == nullptr) {
        return;
    }

    ++animation_tick_;

    // While music is playing, animate only the equalizer.
    if (music_mode_) {
        UpdateMusicVisualizer();
        return;
    }

    constexpr int face_width = 128;
    constexpr int eye_width = 44;
    constexpr int eye_height = 36;
    constexpr int eye_gap = 12;

    const int pair_width =
        eye_width * 2 + eye_gap;

    const int base_x =
        (face_width - pair_width) / 2;

    const int base_y =
        (48 - eye_height) / 2;

    constexpr int max_look_x = 7;
    constexpr int max_look_y = 4;

    if (animation_tick_ >= next_blink_tick_) {
        blink_until_tick_ = animation_tick_ + 3;

        next_blink_tick_ =
            animation_tick_ + 70 + (NextRandom() % 100);
    }

    const bool blink =
        animation_tick_ < blink_until_tick_;

    if (animation_tick_ >= next_idle_tick_ &&
        face_mode_ == FaceMode::kNeutral) {
        target_look_x_ =
            static_cast<int>(
                NextRandom() % (max_look_x * 2 + 1)) -
            max_look_x;

        target_look_y_ =
            static_cast<int>(
                NextRandom() % (max_look_y * 2 + 1)) -
            max_look_y;

        next_idle_tick_ =
            animation_tick_ + 45 + (NextRandom() % 75);
    }

    if (face_mode_ == FaceMode::kListening) {
        target_look_x_ = 0;
        target_look_y_ =
            ((animation_tick_ / 8) % 2 == 0) ? -2 : 1;
    } else if (face_mode_ == FaceMode::kThinking) {
        target_look_x_ = max_look_x;
        target_look_y_ = -max_look_y;
    } else if (face_mode_ == FaceMode::kAngry) {
        target_look_x_ = 0;
        target_look_y_ = 2;
    } else if (
        face_mode_ == FaceMode::kSpeaking ||
        face_mode_ == FaceMode::kHappy) {
        target_look_x_ = 0;
        target_look_y_ = 0;
    }

    current_look_x_ =
        Ease(current_look_x_, target_look_x_);

    current_look_y_ =
        Ease(current_look_y_, target_look_y_);

    int target_height = eye_height;

    if (blink) {
        target_height = 2;
    } else if (face_mode_ == FaceMode::kSleepy) {
        // Half-closed sleepy eyes.
        target_height = 7;
    } else if (face_mode_ == FaceMode::kListening) {
        target_height = eye_height + 3;
    }

    current_eye_height_ =
        Ease(current_eye_height_, target_height);

    int animation_x = 0;
    int animation_y = 0;

    if (face_mode_ == FaceMode::kSpeaking) {
        animation_y =
            ((animation_tick_ / 3) % 2 == 0) ? -1 : 1;
    } else if (face_mode_ == FaceMode::kThinking) {
        animation_x =
            ((animation_tick_ / 4) % 2 == 0) ? -1 : 1;
    }

    const int left_x =
        base_x + current_look_x_ + animation_x;

    const int right_x =
        left_x + eye_width + eye_gap;

    const int eye_y =
        base_y +
        (eye_height - current_eye_height_) / 2 +
        current_look_y_ +
        animation_y;

    lv_obj_set_pos(left_eye_, left_x, eye_y);
    lv_obj_set_size(
        left_eye_, eye_width, current_eye_height_);

    lv_obj_set_pos(right_eye_, right_x, eye_y);
    lv_obj_set_size(
        right_eye_, eye_width, current_eye_height_);

    for (auto* lid : {
             left_top_lid_,
             right_top_lid_,
             left_bottom_lid_,
             right_bottom_lid_}) {
        lv_obj_add_flag(lid, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_transform_rotation(lid, 0, 0);
    }

    constexpr int lid_height = 10;

    if (!blink &&
        (face_mode_ == FaceMode::kAngry ||
         face_mode_ == FaceMode::kSad)) {
        const bool angry =
            face_mode_ == FaceMode::kAngry;

        const int lid_width = eye_width + 8;

        lv_obj_remove_flag(
            left_top_lid_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(
            right_top_lid_, LV_OBJ_FLAG_HIDDEN);

        lv_obj_set_pos(
            left_top_lid_, left_x - 4, eye_y - 2);
        lv_obj_set_size(
            left_top_lid_, lid_width, lid_height);

        lv_obj_set_pos(
            right_top_lid_, right_x - 4, eye_y - 2);
        lv_obj_set_size(
            right_top_lid_, lid_width, lid_height);

        lv_obj_set_style_transform_rotation(
            left_top_lid_, angry ? -140 : 140, 0);
        lv_obj_set_style_transform_rotation(
            right_top_lid_, angry ? 140 : -140, 0);
    }

    if (!blink &&
        (face_mode_ == FaceMode::kHappy ||
         face_mode_ == FaceMode::kSpeaking)) {
        constexpr int cover_height = 17;

        lv_obj_remove_flag(
            left_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(
            right_bottom_lid_, LV_OBJ_FLAG_HIDDEN);

        lv_obj_set_pos(
            left_bottom_lid_,
            left_x - 2,
            eye_y + current_eye_height_ -
                cover_height / 2);

        lv_obj_set_size(
            left_bottom_lid_,
            eye_width + 4,
            cover_height);

        lv_obj_set_pos(
            right_bottom_lid_,
            right_x - 2,
            eye_y + current_eye_height_ -
                cover_height / 2);

        lv_obj_set_size(
            right_bottom_lid_,
            eye_width + 4,
            cover_height);
    }
}

void PocketOledDisplay::SetMusicMode(bool enabled) {
    if (music_mode_ == enabled) {
        return;
    }

    music_mode_ = enabled;

    if (face_layer_ == nullptr) {
        return;
    }

    // Hide normal eyes and eyelids during music playback.
    for (auto* object : {
             left_eye_,
             right_eye_,
             left_top_lid_,
             right_top_lid_,
             left_bottom_lid_,
             right_bottom_lid_}) {
        if (object == nullptr) {
            continue;
        }

        if (enabled) {
            lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Show or hide the equalizer bars.
    for (auto* bar : music_bars_) {
        if (bar == nullptr) {
            continue;
        }

        if (enabled) {
            lv_obj_remove_flag(bar, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (enabled) {
        music_last_update_ = 0;

        for (int i = 0; i < kMusicBarCount; ++i) {
            music_heights_[i] = 5;
            music_targets_[i] = 5;
        }

        UpdateMusicVisualizer();
    } else {
        // Restore the original face animation.
        UpdateAnimationFrame();
    }
}

void PocketOledDisplay::UpdateMusicVisualizer() {
    if (!music_mode_ || face_layer_ == nullptr) {
        return;
    }

    const uint32_t now = lv_tick_get();

    // Choose new equalizer heights every 75 ms.
    if (music_last_update_ == 0 ||
        now - music_last_update_ >= kMusicUpdatePeriodMs) {
        music_last_update_ = now;

        const bool beat =
            ((now / kMusicUpdatePeriodMs) % 4) == 0;

        for (int i = 0; i < kMusicBarCount; ++i) {
            int target = 5;

            if (i >= 3 && i <= 5) {
                // Middle bars.
                target =
                    6 + static_cast<int>(NextRandom() % 18);
            } else if (i <= 2) {
                // Left-side bars.
                target = beat
                    ? 16 + static_cast<int>(NextRandom() % 12)
                    : 5 + static_cast<int>(NextRandom() % 9);
            } else {
                // Right-side bars.
                target =
                    4 + static_cast<int>(NextRandom() % 15);
            }

            music_targets_[i] =
                std::max(4, std::min(28, target));
        }
    }

    constexpr int bar_width = 6;
    constexpr int bar_step = 9;
    constexpr int first_x =
        (128 - (kMusicBarCount * bar_step - 3)) / 2;
    constexpr int baseline_y = 43;

    for (int i = 0; i < kMusicBarCount; ++i) {
        if (music_bars_[i] == nullptr) {
            continue;
        }

        // Smooth movement instead of instantly jumping.
        if (music_heights_[i] < music_targets_[i]) {
            music_heights_[i] =
                std::min(
                    music_heights_[i] + 3,
                    music_targets_[i]);
        } else if (
            music_heights_[i] > music_targets_[i]) {
            music_heights_[i] =
                std::max(
                    music_heights_[i] - 2,
                    music_targets_[i]);
        }

        const int height =
            std::max(3, music_heights_[i]);

        lv_obj_set_size(
            music_bars_[i], bar_width, height);

        lv_obj_set_pos(
            music_bars_[i],
            first_x + i * bar_step,
            baseline_y - height);
    }
}

void PocketOledDisplay::SetFaceMode(FaceMode mode) {
    face_mode_ = mode;

    if (mode != FaceMode::kNeutral) {
        target_look_x_ = 0;
        target_look_y_ = 0;
    }

    UpdateAnimationFrame();
}

PocketOledDisplay::FaceMode
PocketOledDisplay::FaceModeFromEmotion(
    const char* emotion) const {
    if (emotion == nullptr) {
        return FaceMode::kNeutral;
    }

    if (Contains(emotion, "happy") ||
        Contains(emotion, "laugh") ||
        Contains(emotion, "lov") ||
        Contains(emotion, "wink") ||
        Contains(emotion, "confident")) {
        return FaceMode::kHappy;
    }

    if (Contains(emotion, "sad") ||
        Contains(emotion, "cry")) {
        return FaceMode::kSad;
    }

    if (Contains(emotion, "angry") ||
        Contains(emotion, "rage")) {
        return FaceMode::kAngry;
    }

    if (Contains(emotion, "sleep") ||
        Contains(emotion, "tired")) {
        return FaceMode::kSleepy;
    }

    if (Contains(emotion, "think") ||
        Contains(emotion, "confused")) {
        return FaceMode::kThinking;
    }

    if (Contains(emotion, "surpris")) {
        return FaceMode::kListening;
    }

    return FaceMode::kNeutral;
}

uint32_t PocketOledDisplay::NextRandom() {
    random_state_ =
        random_state_ * 1664525U + 1013904223U;

    return random_state_;
}
