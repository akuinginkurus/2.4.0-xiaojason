#include "pocket_oled_display.h"

#include "assets/lang_config.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <initializer_list>

namespace {

constexpr uint32_t kAnimationPeriodMs = 40;
constexpr uint32_t kMusicUpdatePeriodMs = 75;
constexpr int kSleepyEyeOffsetY = 14;

// Mode tes: 12 detik pertama setelah boot wajah bergantian
// angry -> sad -> normal. Ubah ke false bila sudah benar.
constexpr bool kFaceDemo = false;

// Bentuk mata ANGRY / SAD: badan mata (rounded) dipotong miring
// oleh potongan persegi hitam bertingkat halus, tanpa rotation.
constexpr int kSlantDrop = 8;        // selisih tinggi sisi luar vs dalam
constexpr int kSlantStrips = 6;     // jumlah potongan penutup per mata
constexpr int kSlantBodyExtra = 9;   // badan mata dinaikkan agar sudut atas
                                     // yang membulat tersembunyi di balik garis
constexpr int kAngryEyeHeight = 28;
constexpr int kSadEyeHeight = 24;

// Ekspresi dari emosi ditahan sebentar supaya tidak langsung ditimpa
// oleh status Berbicara / Mendengarkan.
constexpr uint32_t kEmotionHoldMs = 12000;
uint32_t g_hold_until = 0;
bool g_hold_active = false;
bool g_from_emotion = false;

bool HoldActive() {
    return g_hold_active &&
           static_cast<int32_t>(g_hold_until - lv_tick_get()) > 0;
}

// Deteksi emosi dari teks percakapan, untuk kasus ketika server
// memanggil tool express_emotion tanpa mengirim pesan emosi ke perangkat.
// 0 = tidak ada, 1 = sedih, 2 = marah
int DetectEmotionFromText(const char* role, const char* content) {
    if (role == nullptr || content == nullptr || content[0] == '\0') {
        return 0;
    }
    std::string text(content);
    for (auto& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    auto contains = [&text](const char* word) {
        return text.find(word) != std::string::npos;
    };

    if (std::strcmp(role, "user") == 0) {
        const bool asks =
            contains("ekspresi") || contains("wajah") ||
            contains("tunjukkan") || contains("tunjukan");
        if (asks && contains("sedih")) {
            return 1;
        }
        if (asks && (contains("marah") || contains("kesal"))) {
            return 2;
        }
        return 0;
    }

    if (std::strcmp(role, "assistant") == 0) {
        if (contains("hiks") || contains("sedih")) {
            return 1;
        }
        if (contains("marah") || contains("kesal") || contains("grr")) {
            return 2;
        }
    }
    return 0;
}

lv_obj_t* g_left_strips[kSlantStrips] = {nullptr};
lv_obj_t* g_right_strips[kSlantStrips] = {nullptr};

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

void HideStrips(lv_obj_t** strips) {
    for (int i = 0; i < kSlantStrips; ++i) {
        if (strips[i] != nullptr) {
            lv_obj_add_flag(strips[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// Pasang penutup miring di atas satu mata.
// top_y     : y tepi atas mata di sisi yang paling tinggi
// left_high : true bila sisi kiri mata yang lebih tinggi
void ApplySlant(
    lv_obj_t** strips,
    int eye_x,
    int eye_w,
    int top_y,
    bool left_high) {
    const int strip_w = (eye_w + kSlantStrips - 1) / kSlantStrips;
    const int cover_top = top_y - kSlantBodyExtra;
    for (int i = 0; i < kSlantStrips; ++i) {
        if (strips[i] == nullptr) {
            continue;
        }
        const int idx = left_high ? i : (kSlantStrips - 1 - i);
        const int edge =
            top_y + (kSlantDrop * (2 * idx + 1)) / (2 * kSlantStrips);
        const int x = eye_x + i * strip_w;
        const int w = std::min(strip_w, eye_w - i * strip_w);
        lv_obj_set_pos(strips[i], x, cover_top);
        lv_obj_set_size(strips[i], w, edge - cover_top);
        lv_obj_remove_flag(strips[i], LV_OBJ_FLAG_HIDDEN);
    }
}

} // namespace

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
    // Potongan penutup adalah anak face_layer_, ikut terhapus.
    for (int i = 0; i < kSlantStrips; ++i) {
        g_left_strips[i] = nullptr;
        g_right_strips[i] = nullptr;
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

    for (int i = 0; i < kMusicBarCount; ++i) {
        music_bars_[i] = lv_obj_create(face_layer_);
        PrepareShape(music_bars_[i]);
        lv_obj_set_style_bg_color(music_bars_[i], lv_color_white(), 0);
        lv_obj_set_style_bg_opa(music_bars_[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(music_bars_[i], 2, 0);
        lv_obj_add_flag(music_bars_[i], LV_OBJ_FLAG_HIDDEN);
        music_heights_[i] = 5;
        music_targets_[i] = 5;
    }

    for (auto* lid : {
             left_top_lid_,
             right_top_lid_,
             left_bottom_lid_,
             right_bottom_lid_}) {
        PrepareShape(lid);
        lv_obj_set_style_bg_color(lid, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(lid, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(lid, 6, 0);
        lv_obj_add_flag(lid, LV_OBJ_FLAG_HIDDEN);
    }

    // Potongan penutup miring untuk ekspresi ANGRY / SAD.
    for (int i = 0; i < kSlantStrips; ++i) {
        for (auto** slot : {&g_left_strips[i], &g_right_strips[i]}) {
            *slot = lv_obj_create(face_layer_);
            PrepareShape(*slot);
            lv_obj_set_style_bg_color(*slot, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(*slot, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(*slot, 0, 0);
            lv_obj_set_style_border_width(*slot, 0, 0);
            lv_obj_add_flag(*slot, LV_OBJ_FLAG_HIDDEN);
        }
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
    standby_mode_ = is_standby;

    if (Contains(status, "Memutar lagu") ||
        Contains(status, "memutar lagu")) {
        SetMusicMode(true);
    } else if (Contains(status, "Mendengarkan") ||
               Contains(status, "mendengarkan") ||
               is_standby) {
        SetMusicMode(false);
    }

    if (music_mode_) {
        if (!has_chat_text_ && chat_line_ != nullptr) {
            lv_label_set_text(chat_line_, status);
        }
        return;
    }

    if (std::strcmp(status, Lang::Strings::LISTENING) == 0) {
        SetFaceMode(FaceMode::kListening);
    } else if (std::strcmp(status, Lang::Strings::SPEAKING) == 0) {
        SetFaceMode(FaceMode::kSpeaking);
    } else if (
        std::strcmp(status, Lang::Strings::CONNECTING) == 0 ||
        Contains(status, "Đang") ||
        Contains(status, "đang")) {
        SetFaceMode(FaceMode::kThinking);
    } else if (is_standby) {
        SetFaceMode(FaceMode::kSleepy);
    }

    if (!has_chat_text_ && chat_line_ != nullptr) {
        lv_label_set_text(chat_line_, status);
    }
}

void PocketOledDisplay::SetEmotion(const char* emotion) {
    OledDisplay::SetEmotion(emotion);

    DisplayLockGuard lock(this);

    if (music_mode_) {
        return;
    }

    if (standby_mode_ &&
        (emotion == nullptr ||
         std::strcmp(emotion, "neutral") == 0)) {
        SetFaceMode(FaceMode::kSleepy);
        return;
    }

    const FaceMode mode = FaceModeFromEmotion(emotion);

    if (mode == FaceMode::kNeutral) {
        // Emosi netral tidak boleh menimpa ekspresi yang sedang ditahan.
        if (HoldActive()) {
            return;
        }
        SetFaceMode(mode);
        return;
    }

    g_hold_active = true;
    g_hold_until = lv_tick_get() + kEmotionHoldMs;
    g_from_emotion = true;
    SetFaceMode(mode);
    g_from_emotion = false;
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

    if (!music_mode_) {
        const int detected = DetectEmotionFromText(role, content);
        if (detected != 0) {
            g_hold_active = true;
            g_hold_until = lv_tick_get() + kEmotionHoldMs;
            g_from_emotion = true;
            SetFaceMode(
                detected == 1 ? FaceMode::kSad : FaceMode::kAngry);
            g_from_emotion = false;
        }
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

    if (music_mode_) {
        UpdateMusicVisualizer();
        return;
    }

    if (g_hold_active && !HoldActive()) {
        g_hold_active = false;
        face_mode_ =
            standby_mode_ ? FaceMode::kSleepy : FaceMode::kNeutral;
    }

    if (kFaceDemo && animation_tick_ < 300) {
        const uint32_t phase = (animation_tick_ / 75) % 4;
        face_mode_ = phase == 0 ? FaceMode::kAngry
                   : phase == 1 ? FaceMode::kSad
                                : FaceMode::kNeutral;
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
        next_blink_tick_ =
            animation_tick_ + 70 + (NextRandom() % 100);
    }

    const bool blink = animation_tick_ < blink_until_tick_;

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
    } else if (face_mode_ == FaceMode::kSad) {
        target_look_x_ = 0;
        target_look_y_ = 2;
    } else if (face_mode_ == FaceMode::kSleepy) {
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
    } else if (face_mode_ == FaceMode::kAngry) {
        target_height = kAngryEyeHeight;
    } else if (face_mode_ == FaceMode::kSad) {
        target_height = kSadEyeHeight;
    } else if (face_mode_ == FaceMode::kListening) {
        target_height = eye_height + 3;
    }

    current_eye_height_ = Ease(current_eye_height_, target_height);

    int animation_x = 0;
    int animation_y = 0;
    if (face_mode_ == FaceMode::kSpeaking) {
        animation_y =
            ((animation_tick_ / 3) % 2 == 0) ? -1 : 1;
    } else if (face_mode_ == FaceMode::kThinking) {
        animation_x =
            ((animation_tick_ / 4) % 2 == 0) ? -1 : 1;
    }

    const int left_x = base_x + current_look_x_ + animation_x;
    const int right_x = left_x + eye_width + eye_gap;

    const int sleepy_offset =
        face_mode_ == FaceMode::kSleepy
            ? kSleepyEyeOffsetY
            : 0;

    const int eye_y =
        base_y +
        (eye_height - current_eye_height_) / 2 +
        current_look_y_ +
        animation_y +
        sleepy_offset;

    lv_obj_set_pos(left_eye_, left_x, eye_y);
    lv_obj_set_size(left_eye_, eye_width, current_eye_height_);
    lv_obj_set_pos(right_eye_, right_x, eye_y);
    lv_obj_set_size(right_eye_, eye_width, current_eye_height_);

    for (auto* lid : {
             left_top_lid_,
             right_top_lid_,
             left_bottom_lid_,
             right_bottom_lid_}) {
        lv_obj_add_flag(lid, LV_OBJ_FLAG_HIDDEN);
    }
    HideStrips(g_left_strips);
    HideStrips(g_right_strips);

    // ANGRY: sisi atas miring turun ke arah tengah (alis marah).
    // SAD  : sisi atas miring naik ke arah tengah (alis sedih).
    // Bawah mata tetap membulat. Tanpa rotation.
    if (!blink &&
        (face_mode_ == FaceMode::kAngry ||
         face_mode_ == FaceMode::kSad)) {
        const bool angry = face_mode_ == FaceMode::kAngry;

        // eye_y = tepi atas sisi tertinggi, bawah = eye_y + tinggi.
        // Badan mata dinaikkan agar sudut atas yang membulat
        // tertutup garis miring.
        const int body_y = eye_y - kSlantBodyExtra;
        const int body_h = current_eye_height_ + kSlantBodyExtra;

        lv_obj_set_pos(left_eye_, left_x, body_y);
        lv_obj_set_size(left_eye_, eye_width, body_h);
        lv_obj_set_pos(right_eye_, right_x, body_y);
        lv_obj_set_size(right_eye_, eye_width, body_h);

        // Angry: sisi luar tinggi (mata kiri = sisi kiri tinggi).
        // Sad  : sisi dalam tinggi (mata kiri = sisi kanan tinggi).
        ApplySlant(g_left_strips, left_x, eye_width, eye_y, angry);
        ApplySlant(g_right_strips, right_x, eye_width, eye_y, !angry);
    }

    // Ekspresi Happy dan Speaking tetap dipertahankan.
    if (!blink &&
        (face_mode_ == FaceMode::kHappy ||
         face_mode_ == FaceMode::kSpeaking)) {
        constexpr int cover_height = 17;

        lv_obj_remove_flag(left_bottom_lid_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(right_bottom_lid_, LV_OBJ_FLAG_HIDDEN);

        lv_obj_set_pos(
            left_bottom_lid_,
            left_x - 2,
            eye_y + current_eye_height_ - cover_height / 2);
        lv_obj_set_size(
            left_bottom_lid_, eye_width + 4, cover_height);

        lv_obj_set_pos(
            right_bottom_lid_,
            right_x - 2,
            eye_y + current_eye_height_ - cover_height / 2);
        lv_obj_set_size(
            right_bottom_lid_, eye_width + 4, cover_height);
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

    // Potongan penutup selalu disembunyikan di sini;
    // UpdateAnimationFrame menampilkannya lagi bila perlu.
    HideStrips(g_left_strips);
    HideStrips(g_right_strips);

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
        UpdateAnimationFrame();
    }
}

void PocketOledDisplay::UpdateMusicVisualizer() {
    if (!music_mode_ || face_layer_ == nullptr) {
        return;
    }

    const uint32_t now = lv_tick_get();

    if (music_last_update_ == 0 ||
        now - music_last_update_ >= kMusicUpdatePeriodMs) {
        music_last_update_ = now;

        const bool beat =
            ((now / kMusicUpdatePeriodMs) % 4) == 0;

        for (int i = 0; i < kMusicBarCount; ++i) {
            int target = 5;

            if (i >= 3 && i <= 5) {
                target =
                    6 + static_cast<int>(NextRandom() % 18);
            } else if (i <= 2) {
                target = beat
                    ? 16 + static_cast<int>(NextRandom() % 12)
                    : 5 + static_cast<int>(NextRandom() % 9);
            } else {
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

        if (music_heights_[i] < music_targets_[i]) {
            music_heights_[i] =
                std::min(
                    music_heights_[i] + 3,
                    music_targets_[i]);
        } else if (music_heights_[i] > music_targets_[i]) {
            music_heights_[i] =
                std::max(
                    music_heights_[i] - 2,
                    music_targets_[i]);
        }

        const int height = std::max(3, music_heights_[i]);

        lv_obj_set_size(music_bars_[i], bar_width, height);
        lv_obj_set_pos(
            music_bars_[i],
            first_x + i * bar_step,
            baseline_y - height);
    }
}

void PocketOledDisplay::SetFaceMode(FaceMode mode) {
    // Status (Berbicara/Mendengarkan/dll) tidak boleh menimpa
    // ekspresi emosi yang sedang ditahan.
    if (!g_from_emotion && HoldActive()) {
        return;
    }
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
        Contains(emotion, "sedih") ||
        Contains(emotion, "cry")) {
        return FaceMode::kSad;
    }

    if (Contains(emotion, "angry") ||
        Contains(emotion, "marah") ||
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
