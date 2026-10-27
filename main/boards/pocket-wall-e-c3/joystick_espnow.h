

#ifndef POCKET_WALL_E_C3_JOYSTICK_ESPNOW_H
#define POCKET_WALL_E_C3_JOYSTICK_ESPNOW_H

#include "motor_controller.h"

#include <cstring>
#include <cstdint>

#include <esp_log.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_timer.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class JoystickEspNow {
private:
    static constexpr const char* TAG = "JoystickESPNow";
    static constexpr uint32_t PACKET_MAGIC = 0x57414C45UL;

    static constexpr uint8_t TYPE_HELLO = 1;
    static constexpr uint8_t TYPE_CONTROL = 2;
    static constexpr uint8_t TYPE_ACK = 3;

    static constexpr uint32_t FAILSAFE_MS = 350;

    struct __attribute__((packed)) JoystickPacket {
        uint32_t magic;
        uint8_t type;
        int8_t left;
        int8_t right;
        uint8_t button;
        uint8_t reserved;
    };

    MotorController* motor_;
    volatile uint32_t lastControlMs_ = 0;
    volatile bool running_ = false;

    static JoystickEspNow* instance_;

    static void ReceiveCallback(
        const esp_now_recv_info_t* info,
        const uint8_t* data,
        int length) {
        if (instance_ != nullptr) {
            instance_->OnReceive(info, data, length);
        }
    }

    static void TaskEntry(void* arg) {
        auto* self = static_cast<JoystickEspNow*>(arg);
        self->TaskLoop();
        vTaskDelete(nullptr);
    }

    bool AddPeer(const uint8_t* mac) {
        if (esp_now_is_peer_exist(mac)) {
            return true;
        }

        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, mac, 6);
        peer.channel = 0;
        peer.ifidx = WIFI_IF_STA;
        peer.encrypt = false;

        esp_err_t err = esp_now_add_peer(&peer);

        if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
            ESP_LOGW(TAG, "Gagal menambahkan peer: %s",
                     esp_err_to_name(err));
            return false;
        }

        return true;
    }

    void SendAck(const uint8_t* mac) {
        JoystickPacket ack = {};
        ack.magic = PACKET_MAGIC;
        ack.type = TYPE_ACK;

        esp_now_send(mac,
                     reinterpret_cast<const uint8_t*>(&ack),
                     sizeof(ack));
    }

    void OnReceive(
        const esp_now_recv_info_t* info,
        const uint8_t* data,
        int length) {
        if (info == nullptr ||
            data == nullptr ||
            length != sizeof(JoystickPacket)) {
            return;
        }

        JoystickPacket packet;
        memcpy(&packet, data, sizeof(packet));

        if (packet.magic != PACKET_MAGIC) {
            return;
        }

        if (packet.type == TYPE_HELLO) {
            if (AddPeer(info->src_addr)) {
                SendAck(info->src_addr);
            }
            return;
        }

        if (packet.type != TYPE_CONTROL ||
            !esp_now_is_peer_exist(info->src_addr)) {
            return;
        }

        // ACK setiap paket kontrol agar remote mengetahui
        // bahwa kanal komunikasi masih aktif.
        SendAck(info->src_addr);

        lastControlMs_ =
            static_cast<uint32_t>(esp_timer_get_time() / 1000);

        if (packet.button ||
            (packet.left == 0 && packet.right == 0)) {
            motor_->Stop();
            return;
        }

        // MotorController yang ada membatasi durasi perintah.
        // Paket berikutnya memperbarui perintah gerak.
        motor_->Drive(
            static_cast<int>(packet.left),
            static_cast<int>(packet.right),
            250);
    }

    void TaskLoop() {
        // Jangan menginisialisasi ulang Wi-Fi.
        // WifiBoard milik XiaoZhi yang mengelolanya.
        wifi_mode_t mode = WIFI_MODE_NULL;

        while (esp_wifi_get_mode(&mode) != ESP_OK ||
               mode == WIFI_MODE_NULL) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        esp_err_t err = esp_now_init();
        if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
            ESP_LOGE(TAG, "esp_now_init gagal: %s",
                     esp_err_to_name(err));
            motor_->Stop();
            vTaskDelete(nullptr);
            return;
        }

        instance_ = this;

        err = esp_now_register_recv_cb(ReceiveCallback);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Gagal mendaftarkan callback");
            motor_->Stop();
            vTaskDelete(nullptr);
            return;
        }

        running_ = true;
        ESP_LOGI(TAG, "Penerima joystick siap");

        while (true) {
            uint32_t now =
                static_cast<uint32_t>(esp_timer_get_time() / 1000);

            uint32_t last = lastControlMs_;

            if (last != 0 &&
                static_cast<uint32_t>(now - last) > FAILSAFE_MS) {
                motor_->Stop();
                lastControlMs_ = 0;
                ESP_LOGW(TAG, "Joystick terputus: motor dihentikan");
            }

            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

public:
    explicit JoystickEspNow(MotorController* motor)
        : motor_(motor) {
        instance_ = this;

        xTaskCreate(
            TaskEntry,
            "joystick_espnow",
            4096,
            this,
            5,
            nullptr);
    }

    ~JoystickEspNow() {
        motor_->Stop();
        running_ = false;
        if (instance_ == this) {
            instance_ = nullptr;
        }
    }
};

inline JoystickEspNow* JoystickEspNow::instance_ = nullptr;

#endif
