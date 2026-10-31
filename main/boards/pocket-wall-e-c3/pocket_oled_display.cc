#include "pocket_oled_display.h"

#include "assets/lang_config.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
// An original LVGL implementation inspired by the visual behavior documented by
// FluxGarage RoboEyes: https://github.com/FluxGarage/RoboEyes
constexpr uint32_t kAnimationPeriodMs = 40;

// Tata letak layar radio (OLED 128x64).
constexpr int kRadioBarWidth = 4;
constexpr int kRadioBarStep = 6;
constexpr int kRadioBarLeft = 9;
constexpr int kRadioBarBottom = 56;
constexpr int kRadioBarMaxHeight = 40;
constexpr int kRadioDialLeft = 46;
constexpr int kRadioDialWidth = 76;
constexpr int kRadioDialTicks = 10;

void PrepareShape(lv_obj_t* object) {
    lv_obj_remove_style_all(object);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

bool Contains(const char* text, const char* value) {
    return text != nullptr && value != nullptr && std::strstr(text, value) != nullptr;
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

// Font besar untuk angka frekuensi: pakai yang tersedia di konfigurasi LVGL,
// kalau tidak ada satu pun, pakai font bawaan.
const lv_font_t* PickBigFont() {
#if defined(LV_FONT_MONTSERRAT_24) && LV_FONT_MONTSERRAT_24
    return &lv_font_montserrat_24;
#elif defined(LV_FONT_MONTSERRAT_20) && LV_FONT_MONTSERRAT_20
    return &lv_font_montserrat_20;
#elif defined(LV_FONT_MONTSERRAT_16) && LV_FONT_MONTSERRAT_16
    return &lv_font_montserrat_16;
#else
    return nullptr;
#endif
}

void StyleLine(lv_obj_t* line, int width) {
    lv_obj_set_style_line_width(line, width, 0);
    lv_obj_set_style_line_color(line, lv_color_white(), 0);
    lv_obj_set_style_line_rounded(line, false, 0);
}
}  // namespace

PocketOledDisplay::PocketOledDisplay(esp_lcd_panel_io_handle_t panel_io,
                                     esp_lcd_panel_handle_t panel, int width, int height,
                                     bool mirror_x, bool mirror_y)
    : OledDisplay(panel_io, panel, width, height, mirror_x, mirror_y) {}

PocketOledDisplay::~PocketOledDisplay() {
    if (animation_timer_ != nullptr) {
        lv_timer_delete(animation_timer_);
        animation_timer_ = nullptr;
    }
    if (radio_layer_ != nullptr) {
        lv_obj_delete(radio_layer_);
        radio_layer_ = nullptr;
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

    face_layer_ = lv_obj_create(screen);
    PrepareShape(face_layer_);
    lv_obj_set_style_bg_color(face_layer_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(face_layer_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(face_layer_, 0, 0);
    lv_obj_set_style_radius(face_layer_, 0, 0);
    lv_obj_set_style_pad_all(face_layer_, 0, 0);

    left_eye_ = lv_obj_create(face_layer_);
    right_eye_ = lv_obj_create(face_layer_);
    left_top_lid_ = lv_obj_create(face_layer_);
    right_top_lid_ = lv_obj_create(face_layer_);
    left_bottom_lid_ = lv_obj_create(face_layer_);
    right_bottom_lid_ = lv_obj_create(face_layer_);

    for (auto* eye : {left_eye_, right_eye_}) {
        PrepareShape(eye);
        lv_obj_set_style_bg_color(eye, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(eye, 8, 0);
    }
    for (auto* lid : {left_top_lid_, right_top_lid_, left_bottom_lid_, right_bottom_lid_}) {
        PrepareShape(lid);
        lv_obj_set_style_bg_color(lid, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(lid, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(lid, 6, 0);
        lv_obj_add_flag(lid, LV_OBJ_FLAG_HIDDEN);
    }

    chat_line_ = lv_label_create(screen);
    lv_obj_set_pos(chat_line_, 0, 48);
    lv_obj_set_size(chat_line_, 128, 16);
    lv_obj_set_style_bg_color(chat_line_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(chat_line_, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(chat_line_, lv_color_white(), 0);
    lv_obj_set_style_text_align(chat_line_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_left(chat_line_, 2, 0);
    lv_obj_set_style_pad_right(chat_line_, 2, 0);
    lv_label_set_long_mode(chat_line_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(chat_line_, "");

    ApplyLayout();
    lv_obj_move_foreground(chat_line_);

    // Layar radio dibuat sekali di sini (saat RAM masih lega) dan hanya
    // disembunyikan/ditampilkan, supaya tidak ada alokasi besar saat radio jalan.
    BuildRadioUI(screen);

    animation_timer_ = lv_timer_create(AnimationTimerCallback, kAnimationPeriodMs, this);
}

void PocketOledDisplay::BuildRadioUI(lv_obj_t* screen) {
    radio_layer_ = lv_obj_create(screen);
    PrepareShape(radio_layer_);
    lv_obj_set_pos(radio_layer_, 0, 0);
    lv_obj_set_size(radio_layer_, 128, 64);
    lv_obj_set_style_bg_color(radio_layer_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(radio_layer_, LV_OPA_COVER, 0);
    lv_obj_add_flag(radio_layer_, LV_OBJ_FLAG_HIDDEN);

    // Badan radio.
    auto* body = lv_obj_create(radio_layer_);
    PrepareShape(body);
    lv_obj_set_pos(body, 0, 6);
    lv_obj_set_size(body, 128, 58);
    lv_obj_set_style_border_width(body, 1, 0);
    lv_obj_set_style_border_color(body, lv_color_white(), 0);
    lv_obj_set_style_border_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(body, 8, 0);

    // Antena.
    static const lv_point_precise_t kAntennaPoints[] = {{104, 6}, {118, 0}};
    auto* antenna = lv_line_create(radio_layer_);
    PrepareShape(antenna);
    lv_line_set_points(antenna, kAntennaPoints, 2);
    StyleLine(antenna, 2);

    // Kotak speaker berisi equalizer.
    auto* speaker = lv_obj_create(radio_layer_);
    PrepareShape(speaker);
    lv_obj_set_pos(speaker, 6, 12);
    lv_obj_set_size(speaker, 32, 46);
    lv_obj_set_style_border_width(speaker, 1, 0);
    lv_obj_set_style_border_color(speaker, lv_color_white(), 0);
    lv_obj_set_style_border_opa(speaker, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(speaker, 4, 0);

    for (int i = 0; i < kRadioBarCount; ++i) {
        radio_bars_[i] = lv_obj_create(radio_layer_);
        PrepareShape(radio_bars_[i]);
        lv_obj_set_style_bg_color(radio_bars_[i], lv_color_white(), 0);
        lv_obj_set_style_bg_opa(radio_bars_[i], LV_OPA_COVER, 0);
        radio_bar_height_[i] = 6;
        lv_obj_set_pos(radio_bars_[i], kRadioBarLeft + i * kRadioBarStep, kRadioBarBottom - 6);
        lv_obj_set_size(radio_bars_[i], kRadioBarWidth, 6);
    }

    // Nama stasiun (bergulir kalau panjang).
    radio_name_ = lv_label_create(radio_layer_);
    lv_obj_set_pos(radio_name_, 44, 10);
    lv_obj_set_width(radio_name_, 78);
    lv_obj_set_style_text_color(radio_name_, lv_color_white(), 0);
    lv_obj_set_style_text_align(radio_name_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(radio_name_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(radio_name_, "");

    // Frekuensi besar.
    radio_freq_ = lv_label_create(radio_layer_);
    lv_obj_set_pos(radio_freq_, 44, 22);
    lv_obj_set_width(radio_freq_, 60);
    lv_obj_set_style_text_color(radio_freq_, lv_color_white(), 0);
    lv_obj_set_style_text_align(radio_freq_, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(radio_freq_, LV_LABEL_LONG_CLIP);
    if (const lv_font_t* big = PickBigFont()) {
        lv_obj_set_style_text_font(radio_freq_, big, 0);
    }
    lv_label_set_text(radio_freq_, "");

    radio_fm_ = lv_label_create(radio_layer_);
    lv_obj_set_pos(radio_fm_, 106, 40);
    lv_obj_set_style_text_color(radio_fm_, lv_color_white(), 0);
    lv_label_set_text(radio_fm_, "FM");

    // Skala dial: garis dasar dengan tanda panjang/pendek.
    static lv_point_precise_t dial_points[kRadioDialTicks * 3 + 1];
    int n = 0;
    for (int k = 0; k < kRadioDialTicks; ++k) {
        const int x = kRadioDialLeft + k * 8;
        const int top = (k % 2 == 0) ? 53 : 55;
        dial_points[n++] = {x, 58};
        dial_points[n++] = {x, top};
        dial_points[n++] = {x, 58};
    }
    dial_points[n++] = {kRadioDialLeft + kRadioDialWidth, 58};
    auto* dial = lv_line_create(radio_layer_);
    PrepareShape(dial);
    lv_line_set_points(dial, dial_points, n);
    StyleLine(dial, 1);

    // Jarum penunjuk frekuensi.
    radio_needle_ = lv_obj_create(radio_layer_);
    PrepareShape(radio_needle_);
    lv_obj_set_style_bg_color(radio_needle_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(radio_needle_, LV_OPA_COVER, 0);
    lv_obj_set_size(radio_needle_, 3, 10);
    lv_obj_set_pos(radio_needle_, kRadioDialLeft, 48);
    lv_obj_add_flag(radio_needle_, LV_OBJ_FLAG_HIDDEN);
}

void PocketOledDisplay::ShowRadio(const char* payload) {
    if (radio_layer_ == nullptr || payload == nullptr) {
        return;
    }

    char name[40] = {0};
    char freq[12] = {0};
    const char* sep = std::strchr(payload, '|');
    if (sep != nullptr) {
        std::snprintf(name, sizeof(name), "%.*s", static_cast<int>(sep - payload), payload);
        std::snprintf(freq, sizeof(freq), "%s", sep + 1);
    } else {
        std::snprintf(name, sizeof(name), "%s", payload);
    }

    const bool numeric = freq[0] >= '0' && freq[0] <= '9';

    lv_label_set_text(radio_name_, name);
    lv_label_set_text(radio_freq_, freq[0] != '\0' ? freq : "LIVE");

    if (numeric) {
        lv_obj_remove_flag(radio_fm_, LV_OBJ_FLAG_HIDDEN);
        // Skala FM 87.5 - 108.0 MHz dipetakan ke lebar dial.
        const float f = static_cast<float>(std::atof(freq));
        float ratio = (f - 87.5f) / (108.0f - 87.5f);
        ratio = std::max(0.0f, std::min(1.0f, ratio));
        const int x = kRadioDialLeft + static_cast<int>(ratio * (kRadioDialWidth - 4));
        lv_obj_set_pos(radio_needle_, x, 48);
        lv_obj_remove_flag(radio_needle_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(radio_fm_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(radio_needle_, LV_OBJ_FLAG_HIDDEN);
    }

    radio_active_ = true;
    lv_obj_remove_flag(radio_layer_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(radio_layer_);
}

void PocketOledDisplay::HideRadio() {
    radio_active_ = false;
    if (radio_layer_ != nullptr) {
        lv_obj_add_flag(radio_layer_, LV_OBJ_FLAG_HIDDEN);
    }
}

void PocketOledDisplay::UpdateRadioFrame() {
    // Equalizer diperbarui tiap 4 tick (±160 ms) supaya ringan untuk CPU dan OLED.
    if (radio_layer_ == nullptr || (animation_tick_ % 4) != 0) {
        return;
    }
    for (int i = 0; i < kRadioBarCount; ++i) {
        const int target = 4 + static_cast<int>((NextRandom() >> 16) % (kRadioBarMaxHeight - 3));
        radio_bar_height_[i] = (radio_bar_height_[i] + target) / 2;
        const int h = std::max(3, std::min(kRadioBarMaxHeight, radio_bar_height_[i]));
        lv_obj_set_pos(radio_bars_[i], kRadioBarLeft + i * kRadioBarStep, kRadioBarBottom - h);
        lv_obj_set_size(radio_bars_[i], kRadioBarWidth, h);
    }
}

void PocketOledDisplay::SetStatus(const char* status) {
    if (status != nullptr && std::strncmp(status, "RADIO|", 6) == 0) {
        OledDisplay::SetStatus("RADIO");
        DisplayLockGuard lock(this);
        ShowRadio(status + 6);
        return;
    }
    if (status != nullptr && std::strcmp(status, "RADIO_END") == 0) {
        OledDisplay::SetStatus(Lang::Strings::STANDBY);
        DisplayLockGuard lock(this);
        HideRadio();
        SetFaceMode(FaceMode::kNeutral);
        if (!has_chat_text_ && chat_line_ != nullptr) {
            lv_label_set_text(chat_line_, Lang::Strings::STANDBY);
        }
        return;
    }

    OledDisplay::SetStatus(status);
    DisplayLockGuard lock(this);

    if (status == nullptr) {
        return;
    }
    // Selama radio diputar, perubahan status lain tidak boleh menimpa layar radio.
    if (radio_active_) {
        return;
    }
    if (std::strcmp(status, Lang::Strings::LISTENING) == 0) {
        SetFaceMode(FaceMode::kListening);
    } else if (std::strcmp(status, Lang::Strings::SPEAKING) == 0) {
        SetFaceMode(FaceMode::kSpeaking);
    } else if (std::strcmp(status, Lang::Strings::CONNECTING) == 0 || Contains(status, "Đang") ||
               Contains(status, "đang")) {
        SetFaceMode(FaceMode::kThinking);
    } else if (std::strcmp(status, Lang::Strings::STANDBY) == 0) {
        SetFaceMode(FaceMode::kNeutral);
    }
    if (!has_chat_text_ && chat_line_ != nullptr) {
        lv_label_set_text(chat_line_, status);
    }
}

void PocketOledDisplay::SetEmotion(const char* emotion) {
    OledDisplay::SetEmotion(emotion);
    DisplayLockGuard lock(this);
    SetFaceMode(FaceModeFromEmotion(emotion));
}

void PocketOledDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);

    has_chat_text_ = content != nullptr && content[0] != '\0';
    if (chat_line_ != nullptr) {
        lv_label_set_text(chat_line_, has_chat_text_ ? content : "");
    }
    if (has_chat_text_ && role != nullptr && std::strcmp(role, "assistant") == 0 &&
        face_mode_ == FaceMode::kNeutral) {
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

void PocketOledDisplay::AnimationTimerCallback(lv_timer_t* timer) {
    auto* display = static_cast<PocketOledDisplay*>(lv_timer_get_user_data(timer));
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
    current_eye_height_ = std::min(current_eye_height_, 36);
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

    // Saat radio diputar, animasi mata dilewati (hemat CPU) dan equalizer yang jalan.
    if (radio_active_) {
        UpdateRadioFrame();
        return;
    }

    constexpr int face_width = 128;
    constexpr int eye_width = 44;
    constexpr int eye_height = 36;
    constexpr int eye_gap = 12;
    const int pair_width = eye_width * 2 + eye_gap;
    const int base_x = (face_width - pair_width) / 2;
    const int base_y = (48 - eye_height) / 2;
    constexpr int max_look_x = 7;
    constexpr int max_look_y = 4;

    if (animation_tick_ >= next_blink_tick_) {
        blink_until_tick_ = animation_tick_ + 3;
        next_blink_tick_ = animation_tick_ + 70 + (NextRandom() % 100);
    }
    const bool blink = animation_tick_ < blink_until_tick_;

    if (animation_tick_ >= next_idle_tick_ && face_mode_ == FaceMode::kNeutral) {
        target_look_x_ = static_cast<int>(NextRandom() % (max_look_x * 2 + 1)) - max_look_x;
        target_look_y_ = static_cast<int>(NextRandom() % (max_look_y * 2 + 1)) - max_look_y;
        next_idle_tick_ = animation_tick_ + 45 + (NextRandom() % 75);
    }

    if (face_mode_ == FaceMode::kListening) {
        target_look_x_ = 0;
        target_look_y_ = ((animation_tick_ / 8) % 2 == 0) ? -2 : 1;
    } else if (face_mode_ == FaceMode::kThinking) {
        target_look_x_ = max_look_x;
        target_look_y_ = -max_look_y;
    } else if (face_mode_ == FaceMode::kAngry) {
        target_look_x_ = 0;
        target_look_y_ = 2;
    } else if (face_mode_ == FaceMode::kSpeaking || face_mode_ == FaceMode::kHappy) {
        target_look_x_ = 0;
        target_look_y_ = 0;
    }

    current_look_x_ = Ease(current_look_x_, target_look_x_);
    current_look_y_ = Ease(current_look_y_, target_look_y_);

    int target_height = eye_height;
    if (blink) {
        target_height = 2;
    } else if (face_mode_ == FaceMode::kSleepy) {
        target_height = 7;
    } else if (face_mode_ == FaceMode::kListening) {
        target_height = eye_height + 3;
    }
    current_eye_height_ = Ease(current_eye_height_, target_height);

    int animation_x = 0;
    int animation_y = 0;
    if (face_mode_ == FaceMode::kSpeaking) {
        // A gentle laugh-like bounce while TTS is active.
        animation_y = ((animation_tick_ / 3) % 2 == 0) ? -1 : 1;
    } else if (face_mode_ == FaceMode::kThinking) {
        // A short horizontal confused-style shiver.
        animation_x = ((animation_tick_ / 4) % 2 == 0) ? -1 : 1;
    }

    const int left_x = base_x + current_look_x_ + animation_x;
    const int right_x = left_x + eye_width + eye_gap;
    const int eye_y =
        base_y + (eye_height - current_eye_height_) / 2 + current_look_y_ + animation_y;

    lv_obj_set_pos(left_eye_, left_x, eye_y);
    lv_obj_set_size(left_eye_, eye_width, current_eye_height_);
    lv_obj_set_pos(right_eye_, right_x, eye_y);
    lv_obj_set_size(right_eye_, eye_width, current_eye_height_);

    for (auto* lid : {left_top_lid_, right_top_lid_, left_bottom_lid_, right_bottom_lid_}) {
        lv_obj_add_flag(lid, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_transform_rotation(lid, 0, 0);
    }

    constexpr int lid_height = 10;
    if (!blink && (face_mode_ == FaceMode::kAngry || face_mode_ == FaceMode::kSad)) {
        const bool angry = face_mode_ == FaceMode::kAngry;
        const int lid_width = eye_width + 8;
        lv_obj_remove_flag(left_top_lid_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(right_top_lid_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(left_top_lid_, left_x - 4, eye_y - 2);
        lv_obj_set_size(left_top_lid_, lid_width, lid_height);
        lv_obj_set_pos(right_top_lid_, right_x - 4, eye_y - 2);
        lv_obj_set_size(right_top_lid_, lid_width, lid_height);
        lv_obj_set_style_transform_rotation(left_top_lid_, angry ? -140 : 140, 0);
        lv_obj_set_style_transform_rotation(right_top_lid_, angry ? 140 : -140, 0);
    }

    if (!blink && (face_mode_ == FaceMode::kHappy || face_mode_ == FaceMode::kSpeaking)) {
        constexpr int cover_height = 17;
        lv_obj_remove_flag(left_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(right_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(left_bottom_lid_, left_x - 2,
                       eye_y + current_eye_height_ - cover_height / 2);
        lv_obj_set_size(left_bottom_lid_, eye_width + 4, cover_height);
        lv_obj_set_pos(right_bottom_lid_, right_x - 2,
                       eye_y + current_eye_height_ - cover_height / 2);
        lv_obj_set_size(right_bottom_lid_, eye_width + 4, cover_height);
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

PocketOledDisplay::FaceMode PocketOledDisplay::FaceModeFromEmotion(const char* emotion) const {
    if (emotion == nullptr) {
        return FaceMode::kNeutral;
    }
    if (Contains(emotion, "happy") || Contains(emotion, "laugh") || Contains(emotion, "lov") ||
        Contains(emotion, "wink") || Contains(emotion, "confident")) {
        return FaceMode::kHappy;
    }
    if (Contains(emotion, "sad") || Contains(emotion, "cry")) {
        return FaceMode::kSad;
    }
    if (Contains(emotion, "angry") || Contains(emotion, "rage")) {
        return FaceMode::kAngry;
    }
    if (Contains(emotion, "sleep") || Contains(emotion, "tired")) {
        return FaceMode::kSleepy;
    }
    if (Contains(emotion, "think") || Contains(emotion, "confused")) {
        return FaceMode::kThinking;
    }
    if (Contains(emotion, "surpris")) {
        return FaceMode::kListening;
    }
    return FaceMode::kNeutral;
}

uint32_t PocketOledDisplay::NextRandom() {
    random_state_ = random_state_ * 1664525U + 1013904223U;
    return random_state_;
}
