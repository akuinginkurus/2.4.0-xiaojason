

#include "pocket_oled_display.h"

#include <cstdio>

void PocketOledDisplay::ShowBatteryStatus(int percentage) {
    DisplayLockGuard lock(this);

    if (face_layer_ == nullptr) {
        return;
    }

    // Create the battery screen only once, on the first request.
    if (battery_overlay_ == nullptr) {
        battery_overlay_ = lv_obj_create(face_layer_);
        lv_obj_remove_style_all(battery_overlay_);
        lv_obj_clear_flag(
            battery_overlay_,
            LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_set_pos(battery_overlay_, 0, 0);
        lv_obj_set_size(battery_overlay_, 128, 48);
        lv_obj_set_style_bg_color(
            battery_overlay_, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(
            battery_overlay_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(
            battery_overlay_, 0, 0);
        lv_obj_set_style_pad_all(
            battery_overlay_, 0, 0);

        // Battery outline.
        battery_icon_body_ = lv_obj_create(battery_overlay_);
        lv_obj_remove_style_all(battery_icon_body_);
        lv_obj_set_pos(battery_icon_body_, 10, 10);
        lv_obj_set_size(battery_icon_body_, 29, 17);
        lv_obj_set_style_border_color(
            battery_icon_body_, lv_color_white(), 0);
        lv_obj_set_style_border_width(
            battery_icon_body_, 2, 0);
        lv_obj_set_style_bg_opa(
            battery_icon_body_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(
            battery_icon_body_, 3, 0);

        // Battery terminal.
        battery_icon_tip_ = lv_obj_create(battery_overlay_);
        lv_obj_remove_style_all(battery_icon_tip_);
        lv_obj_set_pos(battery_icon_tip_, 39, 15);
        lv_obj_set_size(battery_icon_tip_, 4, 7);
        lv_obj_set_style_bg_color(
            battery_icon_tip_, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(
            battery_icon_tip_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(
            battery_icon_tip_, 1, 0);

        // Battery level fill.
        battery_icon_fill_ = lv_obj_create(battery_overlay_);
        lv_obj_remove_style_all(battery_icon_fill_);
        lv_obj_set_pos(battery_icon_fill_, 13, 13);
        lv_obj_set_size(battery_icon_fill_, 23, 11);
        lv_obj_set_style_bg_color(
            battery_icon_fill_, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(
            battery_icon_fill_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(
            battery_icon_fill_, 1, 0);

        // Percentage.
        battery_text_ = lv_label_create(battery_overlay_);
        lv_obj_set_pos(battery_text_, 49, 5);
        lv_obj_set_size(battery_text_, 76, 25);
        lv_obj_set_style_text_color(
            battery_text_, lv_color_white(), 0);
        lv_obj_set_style_text_align(
            battery_text_, LV_TEXT_ALIGN_LEFT, 0);

        // Low battery warning.
        battery_warning_ = lv_label_create(battery_overlay_);
        lv_obj_set_pos(battery_warning_, 1, 29);
        lv_obj_set_size(battery_warning_, 126, 17);
        lv_obj_set_style_text_color(
            battery_warning_, lv_color_white(), 0);
        lv_obj_set_style_text_align(
            battery_warning_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(
            battery_warning_, LV_LABEL_LONG_CLIP);

        lv_obj_add_flag(
            battery_overlay_, LV_OBJ_FLAG_HIDDEN);
    }

    if (percentage < 0) {
        lv_label_set_text(battery_text_, "Battery N/A");
        lv_label_set_text(
            battery_warning_, "ADC UNAVAILABLE");
        lv_obj_add_flag(
            battery_icon_fill_, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (percentage > 100) percentage = 100;
        if (percentage < 0) percentage = 0;

        char text[12];
        std::snprintf(
            text, sizeof(text), "%d%%", percentage);

        lv_label_set_text(battery_text_, text);

        lv_label_set_text(
            battery_warning_,
            percentage <= 20
                ? "WARNING BATTERY LOW"
                : "");

        const int fill_width =
            (23 * percentage + 50) / 100;

        lv_obj_set_size(
            battery_icon_fill_,
            fill_width > 0 ? fill_width : 1,
            11);

        lv_obj_clear_flag(
            battery_icon_fill_, LV_OBJ_FLAG_HIDDEN);
    }

    battery_overlay_active_ = true;

    lv_obj_clear_flag(
        battery_overlay_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_move_foreground(battery_overlay_);

    // Keep the existing bottom text area visible.
    if (chat_line_ != nullptr) {
        lv_obj_move_foreground(chat_line_);
    }

    // Restart the one-shot hide timer if the command is repeated.
    if (battery_hide_timer_ != nullptr) {
        lv_timer_delete(battery_hide_timer_);
        battery_hide_timer_ = nullptr;
    }

    battery_hide_timer_ = lv_timer_create(
        BatteryHideTimerCallback,
        5000,
        this);

    if (battery_hide_timer_ != nullptr) {
        lv_timer_set_repeat_count(battery_hide_timer_, 1);
    }
}


void PocketOledDisplay::BatteryHideTimerCallback(
    lv_timer_t* timer) {

    auto* display =
        static_cast<PocketOledDisplay*>(
            lv_timer_get_user_data(timer));

    if (display != nullptr) {
        DisplayLockGuard lock(display);

        if (display->battery_overlay_ != nullptr) {
            lv_obj_add_flag(
                display->battery_overlay_,
                LV_OBJ_FLAG_HIDDEN);
        }

        display->battery_overlay_active_ = false;
        display->battery_hide_timer_ = nullptr;
    }

    lv_timer_delete(timer);
}
