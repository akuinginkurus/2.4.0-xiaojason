#include "pocket_oled_display.h"

#include "assets/lang_config.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <initializer_list>

// Didefinisikan di motor_controller.cc: gerakkan robot sebentar saat lagu mulai.
extern void PocketRobotSongStartWiggle();

// Didefinisikan di pocket_wall_e_c3.cc: abaikan sensor gerak sementara
// (supaya gerakan robot sendiri tidak dikira diangkat/digoyang).
extern void PocketMotionSuppress(uint32_t ms);

namespace {

constexpr uint32_t kAnimationPeriodMs = 40;
constexpr uint32_t kMusicUpdatePeriodMs = 75;
constexpr int kSleepyEyeOffsetY = 14;

// Mode tes: 12 detik pertama setelah boot wajah bergantian
// angry -> sad -> normal. Ubah ke false bila sudah benar.
constexpr bool kFaceDemo = false;

// Bentuk mata ANGRY / SAD: alis miring digambar sebagai segitiga
// (garis miring presisi, bukan potongan bertingkat), tanpa rotation.
constexpr int kSlantDrop = 8;        // selisih tinggi sisi luar vs dalam
constexpr int kSlantBodyExtra = 9;   // badan mata dinaikkan agar sudut atas
                                     // yang membulat tersembunyi di balik alis
constexpr int kQ = 16;               // satuan Q4: 16 = 1 piksel
constexpr int kAngryEyeHeight = 28;
constexpr int kSadEyeHeight = 24;

// Ekspresi dari emosi ditahan sebentar supaya tidak langsung ditimpa
// oleh status Berbicara / Mendengarkan.
constexpr uint32_t kEmotionHoldMs = 12000;
uint32_t g_hold_until = 0;
bool g_hold_active = false;
bool g_from_emotion = false;

// Permintaan wajah marah dari sensor gerak (task lain). Diisi lewat
// PocketOledRequestAngry() tanpa mengunci LVGL; dibaca oleh timer animasi.
std::atomic<uint32_t> g_angry_request_ms{0};
std::atomic<int> g_angry_kind{1};
bool g_angry_text_active = false;  // teks protes sedang tampil di chat_line_

// Tidur bertahap saat idle (satuan tick animasi = 40 ms):
//   0 .. kSleepSquintTick  : mata masih terbuka
//   .. kSleepClosedTick    : mata menyipit
//   .. kSleepZzzTick       : mata menutup
//   >= kSleepZzzTick       : muncul "z Z Z" yang bergerak
// Ubah angka ini untuk mempercepat / memperlambat.
constexpr uint32_t kSleepSquintTick = 250;  // 10 detik
constexpr uint32_t kSleepClosedTick = 500;  // 20 detik
constexpr uint32_t kSleepZzzTick = 575;     // 23 detik
constexpr uint32_t kZzzStepTicks = 14;      // ~0,56 detik per langkah

uint32_t g_sleep_start_tick = 0;
bool g_prev_sleepy = false;

// Satu label "zzz" memakai teks statis (tanpa alokasi heap saat berganti).
lv_obj_t* g_zzz_label = nullptr;
bool g_zzz_visible = false;
int g_zzz_step = -1;
const char* const kZzzTexts[4] = {"z", "z z", "z Z", "z Z Z"};

void SetZzzVisible(bool visible) {
    if (g_zzz_label == nullptr || g_zzz_visible == visible) {
        return;
    }
    g_zzz_visible = visible;
    if (visible) {
        g_zzz_step = -1;
        lv_obj_remove_flag(g_zzz_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_zzz_label, LV_OBJ_FLAG_HIDDEN);
    }
}

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

struct BrowInfo {
    int drop;        // selisih tinggi miring, dalam piksel
    bool left_high;  // true bila sisi kiri penutup yang lebih tinggi
};

BrowInfo g_left_brow = {0, true};
BrowInfo g_right_brow = {0, false};

void PrepareShape(lv_obj_t* object) {
    lv_obj_remove_style_all(object);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

bool Contains(const char* text, const char* value) {
    return text != nullptr &&
           value != nullptr &&
           std::strstr(text, value) != nullptr;
}

// True bila teks berbentuk jam "HH:MM" (dikirim sistem saat idle).
bool IsClockText(const char* text) {
    return text != nullptr &&
           std::strlen(text) == 5 &&
           std::isdigit(static_cast<unsigned char>(text[0])) &&
           std::isdigit(static_cast<unsigned char>(text[1])) &&
           text[2] == ':' &&
           std::isdigit(static_cast<unsigned char>(text[3])) &&
           std::isdigit(static_cast<unsigned char>(text[4]));
}

// Jam sekarang "HH:MM", atau teks kosong bila waktu belum tersinkron.
std::string CurrentClockText() {
    time_t now = time(nullptr);
    struct tm info;
    localtime_r(&now, &info);
    if (info.tm_year < 2025 - 1900) {
        return "";
    }
    char buffer[8];
    strftime(buffer, sizeof(buffer), "%H:%M", &info);
    return buffer;
}

// Gelombang sinus kasar: hasil -127..127, satu putaran = period tick.
int SinWave(uint32_t tick, uint32_t period) {
    static constexpr int8_t kSin32[32] = {
        0, 25, 49, 71, 90, 106, 117, 125,
        127, 125, 117, 106, 90, 71, 49, 25,
        0, -25, -49, -71, -90, -106, -117, -125,
        -127, -125, -117, -106, -90, -71, -49, -25};
    if (period == 0) {
        return 0;
    }
    return kSin32[((tick % period) * 32) / period];
}

// Mendekatkan nilai ke target secara halus. divisor besar = lebih lambat.
int EaseQ(int current, int target, int divisor) {
    const int delta = target - current;
    if (delta > -2 && delta < 2) {
        return target;
    }
    int step = delta / divisor;
    if (step == 0) {
        step = delta > 0 ? 1 : -1;
    }
    return current + step;
}

// Q4 -> piksel dengan pembulatan ke terdekat (aman untuk nilai negatif).
int RoundQ(int value_q) {
    return value_q >= 0 ? (value_q + kQ / 2) / kQ
                        : -((-value_q + kQ / 2) / kQ);
}

// Menggambar penutup alis: persegi hitam di atas + segitiga hitam miring
// di bawahnya, sehingga tepi miring tepat satu piksel per langkah.
void BrowCoverDrawCb(lv_event_t* e) {
    auto* object = static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto* info = static_cast<BrowInfo*>(lv_event_get_user_data(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    if (object == nullptr || info == nullptr || layer == nullptr) {
        return;
    }

    lv_area_t area;
    lv_obj_get_coords(object, &area);

    const int drop = info->drop;

    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.bg_color = lv_color_black();
    rect_dsc.bg_opa = LV_OPA_COVER;
    rect_dsc.radius = 0;

    lv_area_t rect_area = {area.x1, area.y1, area.x2, area.y2 - drop};
    lv_draw_rect(layer, &rect_dsc, &rect_area);

    if (drop <= 0) {
        return;
    }

    lv_draw_triangle_dsc_t tri_dsc;
    lv_draw_triangle_dsc_init(&tri_dsc);
    tri_dsc.color = lv_color_black();
    tri_dsc.opa = LV_OPA_COVER;

    const int top = area.y2 - drop + 1;
    if (info->left_high) {
        tri_dsc.p[0].x = area.x1;
        tri_dsc.p[0].y = top;
        tri_dsc.p[1].x = area.x2 + 1;
        tri_dsc.p[1].y = top;
        tri_dsc.p[2].x = area.x2 + 1;
        tri_dsc.p[2].y = top + drop;
    } else {
        tri_dsc.p[0].x = area.x2 + 1;
        tri_dsc.p[0].y = top;
        tri_dsc.p[1].x = area.x1;
        tri_dsc.p[1].y = top;
        tri_dsc.p[2].x = area.x1;
        tri_dsc.p[2].y = top + drop;
    }
    lv_draw_triangle(layer, &tri_dsc);
}

} // namespace

// Dipanggil dari task sensor gerak (MPU6050). Hanya mengisi variabel atomik;
// wajah marah diterapkan oleh timer animasi di UpdateAnimationFrame().
void PocketOledRequestAngry(uint32_t hold_ms, int kind) {
    g_angry_kind.store(kind);
    g_angry_request_ms.store(hold_ms == 0 ? 1 : hold_ms);
}

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
    // Penutup alis adalah anak face_layer_, ikut terhapus.
    left_brow_cover_ = nullptr;
    right_brow_cover_ = nullptr;
    g_zzz_label = nullptr;
    g_zzz_visible = false;
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

    // Penutup alis miring untuk ekspresi ANGRY / SAD (digambar segitiga).
    left_brow_cover_ = lv_obj_create(face_layer_);
    right_brow_cover_ = lv_obj_create(face_layer_);
    PrepareShape(left_brow_cover_);
    PrepareShape(right_brow_cover_);
    lv_obj_add_event_cb(
        left_brow_cover_, BrowCoverDrawCb, LV_EVENT_DRAW_MAIN, &g_left_brow);
    lv_obj_add_event_cb(
        right_brow_cover_, BrowCoverDrawCb, LV_EVENT_DRAW_MAIN, &g_right_brow);
    lv_obj_add_flag(left_brow_cover_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(right_brow_cover_, LV_OBJ_FLAG_HIDDEN);

    // Label "zzz" untuk tidur nyenyak; tersembunyi sampai tahap akhir tidur.
    g_zzz_label = lv_label_create(face_layer_);
    lv_obj_set_style_text_color(g_zzz_label, lv_color_white(), 0);
    lv_obj_set_pos(g_zzz_label, 86, 4);
    lv_label_set_text_static(g_zzz_label, kZzzTexts[0]);
    lv_obj_add_flag(g_zzz_label, LV_OBJ_FLAG_HIDDEN);
    g_zzz_visible = false;

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

    // Jam "HH:MM" dikirim sistem saat idle: tampilkan saja, tanpa teks lain.
    if (IsClockText(status)) {
        if (standby_mode_ && !music_mode_ && chat_line_ != nullptr) {
            has_chat_text_ = false;
            lv_label_set_text(chat_line_, status);
        }
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

    if (is_standby) {
        // Saat idle tidak ada teks apa pun selain jam.
        has_chat_text_ = false;
        if (chat_line_ != nullptr) {
            lv_label_set_text(chat_line_, CurrentClockText().c_str());
        }
        return;
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
    const char* raw_content) {
    DisplayLockGuard lock(this);

    // Buang perintah tool ("% self.xxx(...)") dari teks yang tampil di layar.
    // Teks sebelum tanda % tetap ditampilkan; kalau seluruhnya perintah, abaikan.
    const char* content = raw_content;
    std::string clean;
    if (raw_content != nullptr) {
        clean = raw_content;
        size_t pos = 0;
        while ((pos = clean.find('%', pos)) != std::string::npos) {
            size_t q = pos + 1;
            while (q < clean.size() && (clean[q] == ' ' || clean[q] == '\t')) {
                q++;
            }
            if (clean.compare(q, 5, "self.") == 0) {
                clean.erase(pos);
                break;
            }
            pos++;
        }
        while (!clean.empty() &&
               std::isspace(static_cast<unsigned char>(clean.back()))) {
            clean.pop_back();
        }
        if (clean.empty() && raw_content[0] != '\0') {
            return;
        }
        content = clean.c_str();
    }

    // Saat idle layar tidak menampilkan teks percakapan.
    if (standby_mode_ && content != nullptr && content[0] != '\0') {
        return;
    }

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

    eye_height_q_ = std::min(eye_height_q_, 36 * kQ);

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

    // Permintaan wajah marah dari sensor gerak (diangkat / digoyang).
    // Diabaikan saat mode musik agar visualizer tidak terganggu.
    const uint32_t angry_request_ms = g_angry_request_ms.exchange(0);
    if (angry_request_ms != 0 && !music_mode_) {
        g_hold_active = true;
        g_hold_until = lv_tick_get() + angry_request_ms;
        face_mode_ = FaceMode::kAngry;
        target_look_x_ = 0;
        target_look_y_ = 0;

        // Protes di layar: 1 = diangkat, 2 = digoyang.
        if (chat_line_ != nullptr) {
            lv_label_set_text(
                chat_line_,
                g_angry_kind.load() == 2 ? "Aduh, pusing!" : "Turunkan aku!");
            g_angry_text_active = true;
            has_chat_text_ = true;
        }
    }

    if (music_mode_) {
        UpdateMusicVisualizer();
        return;
    }

    if (g_hold_active && !HoldActive()) {
        g_hold_active = false;
        face_mode_ =
            standby_mode_ ? FaceMode::kSleepy : FaceMode::kNeutral;

        // Hapus teks protes; saat idle kembalikan jam.
        if (g_angry_text_active) {
            g_angry_text_active = false;
            has_chat_text_ = false;
            if (chat_line_ != nullptr) {
                lv_label_set_text(
                    chat_line_,
                    standby_mode_ ? CurrentClockText().c_str() : "");
            }
        }
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

    const uint32_t tick = animation_tick_;

    // Hitung sudah berapa lama masuk mode tidur (idle).
    const bool sleepy_now = face_mode_ == FaceMode::kSleepy;
    if (sleepy_now && !g_prev_sleepy) {
        g_sleep_start_tick = tick;
    }
    g_prev_sleepy = sleepy_now;
    const uint32_t sleep_age = sleepy_now ? tick - g_sleep_start_tick : 0;

    // Kedip: kadang sekali, kadang dua kali berturut-turut.
    if (tick >= next_blink_tick_) {
        blink_until_tick_ = tick + 4;
        if ((NextRandom() % 4) == 0) {
            next_blink_tick_ = tick + 9;
        } else {
            next_blink_tick_ = tick + 70 + (NextRandom() % 100);
        }
    }

    const bool blink = tick < blink_until_tick_;

    // Pandangan acak saat diam: kadang kembali ke tengah, kadang melirik.
    if (tick >= next_idle_tick_ &&
        face_mode_ == FaceMode::kNeutral) {
        if ((NextRandom() % 5) < 2) {
            target_look_x_ = 0;
            target_look_y_ = 0;
        } else {
            target_look_x_ =
                static_cast<int>(
                    NextRandom() % (max_look_x * 2 + 1)) -
                max_look_x;
            target_look_y_ =
                static_cast<int>(
                    NextRandom() % (max_look_y * 2 + 1)) -
                max_look_y;
        }
        next_idle_tick_ = tick + 35 + (NextRandom() % 80);
    }

    if (face_mode_ == FaceMode::kListening) {
        // Melirik naik-turun pelan, seolah menyimak.
        target_look_x_ = 0;
        target_look_y_ = SinWave(tick, 48) * 2 / 127 - 1;
    } else if (face_mode_ == FaceMode::kThinking) {
        // Mata menyapu kiri-kanan di atas, seperti sedang berpikir.
        target_look_x_ =
            ((tick / 36) % 2 == 0) ? max_look_x : -max_look_x;
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
        target_look_y_ = 3;
    } else if (face_mode_ == FaceMode::kSleepy) {
        target_look_x_ = 0;
        target_look_y_ = 0;
    }

    look_x_q_ = EaseQ(look_x_q_, target_look_x_ * kQ, 4);
    look_y_q_ = EaseQ(look_y_q_, target_look_y_ * kQ, 4);

    // Tinggi mata (Q4) beserta gerak "bernapas" yang sangat halus.
    int target_height_q = eye_height * kQ;
    if (blink) {
        target_height_q = 2 * kQ;
    } else if (face_mode_ == FaceMode::kSleepy) {
        if (sleep_age < kSleepSquintTick) {
            // Masih terjaga: mata terbuka, bernapas pelan.
            target_height_q =
                eye_height * kQ + SinWave(tick, 100) * (kQ / 2) / 127;
        } else if (sleep_age < kSleepClosedTick) {
            // Mengantuk: menyipit.
            target_height_q = 16 * kQ + SinWave(tick, 60) * 8 / 127;
        } else {
            // Tertidur: mata menutup jadi garis tipis.
            target_height_q = 3 * kQ;
        }
    } else if (face_mode_ == FaceMode::kAngry) {
        target_height_q = kAngryEyeHeight * kQ;
    } else if (face_mode_ == FaceMode::kSad) {
        target_height_q = kSadEyeHeight * kQ;
    } else if (face_mode_ == FaceMode::kListening) {
        target_height_q =
            (eye_height + 3) * kQ + SinWave(tick, 24) * kQ / 127;
    } else if (face_mode_ == FaceMode::kSpeaking) {
        target_height_q =
            eye_height * kQ + SinWave(tick, 10) * (2 * kQ) / 127;
    } else {
        target_height_q =
            eye_height * kQ + SinWave(tick, 100) * (kQ / 2) / 127;
    }

    eye_height_q_ = EaseQ(eye_height_q_, target_height_q, blink ? 2 : 3);
    const int eye_h = std::max(2, RoundQ(eye_height_q_));

    // Gerak tambahan: memantul saat senang, goyang halus saat bicara,
    // gemetar kecil saat marah.
    int bob_y_q = 0;
    int shake_x_q = 0;
    if (face_mode_ == FaceMode::kSpeaking) {
        bob_y_q = SinWave(tick, 14) * kQ / 127;
    } else if (face_mode_ == FaceMode::kHappy) {
        bob_y_q = -(std::abs(SinWave(tick, 16)) * 2 * kQ) / 127;
    } else if (face_mode_ == FaceMode::kAngry) {
        shake_x_q = ((tick / 2) % 2 == 0) ? 0 : kQ;
    } else if (face_mode_ == FaceMode::kThinking) {
        shake_x_q = SinWave(tick, 16) * kQ / 127;
    }

    const int left_x =
        base_x + RoundQ(look_x_q_ + shake_x_q);
    const int right_x = left_x + eye_width + eye_gap;

    // Mata turun perlahan seiring menyipit / menutup.
    const int sleepy_offset =
        face_mode_ == FaceMode::kSleepy
            ? std::clamp(
                  (eye_height - eye_h) * kSleepyEyeOffsetY / (eye_height - 3),
                  0,
                  kSleepyEyeOffsetY)
            : 0;

    const int eye_y =
        base_y +
        (eye_height - eye_h) / 2 +
        RoundQ(look_y_q_ + bob_y_q) +
        sleepy_offset;

    lv_obj_set_pos(left_eye_, left_x, eye_y);
    lv_obj_set_size(left_eye_, eye_width, eye_h);
    lv_obj_set_pos(right_eye_, right_x, eye_y);
    lv_obj_set_size(right_eye_, eye_width, eye_h);

    // "zzz" hanya muncul di tahap akhir tidur.
    const bool show_zzz = sleepy_now && sleep_age >= kSleepZzzTick;
    SetZzzVisible(show_zzz);
    if (show_zzz && g_zzz_label != nullptr) {
        const int step =
            static_cast<int>(((sleep_age - kSleepZzzTick) / kZzzStepTicks) % 4);
        if (step != g_zzz_step) {
            g_zzz_step = step;
            lv_label_set_text_static(g_zzz_label, kZzzTexts[step]);
            lv_obj_set_pos(g_zzz_label, 86, 6 - step * 2);
        }
    }

    for (auto* lid : {
             left_top_lid_,
             right_top_lid_,
             left_bottom_lid_,
             right_bottom_lid_}) {
        lv_obj_add_flag(lid, LV_OBJ_FLAG_HIDDEN);
    }

    // ANGRY: sisi atas miring turun ke arah tengah (alis marah).
    // SAD  : sisi atas miring naik ke arah tengah (alis sedih).
    // Kemiringan tumbuh dan menyusut perlahan saat ekspresi berganti.
    const bool slanted_mode =
        face_mode_ == FaceMode::kAngry ||
        face_mode_ == FaceMode::kSad;

    if (slanted_mode) {
        brow_angry_ = face_mode_ == FaceMode::kAngry;
    }

    brow_q_ = EaseQ(brow_q_, slanted_mode ? kSlantDrop * kQ : 0, 3);
    const int drop = std::min(kSlantDrop, RoundQ(brow_q_));
    const bool show_brow = !blink && (slanted_mode || drop > 0);

    if (show_brow) {
        // Badan mata dinaikkan agar sudut atas yang membulat
        // tertutup alis miring.
        const int body_y = eye_y - kSlantBodyExtra;
        const int body_h = eye_h + kSlantBodyExtra;

        lv_obj_set_pos(left_eye_, left_x, body_y);
        lv_obj_set_size(left_eye_, eye_width, body_h);
        lv_obj_set_pos(right_eye_, right_x, body_y);
        lv_obj_set_size(right_eye_, eye_width, body_h);

        // Angry: sisi luar tinggi (mata kiri = sisi kiri tinggi).
        // Sad  : sisi dalam tinggi (mata kiri = sisi kanan tinggi).
        g_left_brow.drop = drop;
        g_left_brow.left_high = brow_angry_;
        g_right_brow.drop = drop;
        g_right_brow.left_high = !brow_angry_;

        const int cover_h = kSlantBodyExtra + drop;

        lv_obj_set_pos(left_brow_cover_, left_x, body_y);
        lv_obj_set_size(left_brow_cover_, eye_width, cover_h);
        lv_obj_set_pos(right_brow_cover_, right_x, body_y);
        lv_obj_set_size(right_brow_cover_, eye_width, cover_h);

        lv_obj_remove_flag(left_brow_cover_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(right_brow_cover_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(left_brow_cover_);
        lv_obj_invalidate(right_brow_cover_);
    } else {
        lv_obj_add_flag(left_brow_cover_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(right_brow_cover_, LV_OBJ_FLAG_HIDDEN);
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
            eye_y + eye_h - cover_height / 2);
        lv_obj_set_size(
            left_bottom_lid_, eye_width + 4, cover_height);

        lv_obj_set_pos(
            right_bottom_lid_,
            right_x - 2,
            eye_y + eye_h - cover_height / 2);
        lv_obj_set_size(
            right_bottom_lid_, eye_width + 4, cover_height);
    }
}

void PocketOledDisplay::SetMusicMode(bool enabled) {
    if (music_mode_ == enabled) {
        return;
    }

    music_mode_ = enabled;

    // Selama lagu diputar, RAM internal sangat sempit: kurangi frekuensi
    // redraw LVGL (40 ms -> 100 ms) agar CPU dan alokasi sementara berkurang.
    if (animation_timer_ != nullptr) {
        lv_timer_set_period(animation_timer_, enabled ? 100 : kAnimationPeriodMs);
    }

    if (enabled) {
        SetZzzVisible(false);
    }

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

    // Penutup alis selalu disembunyikan di sini;
    // UpdateAnimationFrame menampilkannya lagi bila perlu.
    if (left_brow_cover_ != nullptr) {
        lv_obj_add_flag(left_brow_cover_, LV_OBJ_FLAG_HIDDEN);
    }
    if (right_brow_cover_ != nullptr) {
        lv_obj_add_flag(right_brow_cover_, LV_OBJ_FLAG_HIDDEN);
    }

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
        // Robot bergoyang sebentar saat lagu mulai; lagu sendiri baru terdengar
        // beberapa detik kemudian setelah stream tersambung dan di-buffer.
        // Sensor gerak diabaikan sementara agar goyangan ini tidak dikira
        // robot diangkat / digoyang.
        PocketMotionSuppress(6000);
        PocketRobotSongStartWiggle();
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
