

#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "wifi_board.h"
#include "audio/codecs/no_audio_codec.h"
#include "sesame_controller.h"

#include <esp_log.h>
#include <string>

#define TAG "XiaozhiXSesame"

class XiaozhiXSesameBoard : public WifiBoard {
private:
    Button action_button_;

    void InitializeButton() {
        action_button_.OnClick([this]() {
            auto& app = Application::GetInstance();

            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }

            app.ToggleChatState();
        });

        action_button_.OnLongPress([this]() {
            EnterWifiConfigMode();
        });
    }

    void InitializeSesame() {
        if (!sesameControllerInit()) {
            ESP_LOGE(TAG, "Sesame controller initialization failed");
            return;
        }

        auto& mcp_server = McpServer::GetInstance();

        mcp_server.AddTool(
            "self.sesame.move",
            "Control the Sesame robot with eight MG90S servos. "
            "Supported commands: rest, stand, wave, dance, swim, "
            "point, pushup, bow, cute, freaky, worm, shake, shrug, "
            "dead, crab, forward, backward, left, right, idle. "
            "Indonesian commands are also supported: istirahat, "
            "berdiri, melambai, menari, berenang, menunjuk, push up, "
            "membungkuk, imut, pura-pura mati, kepiting, maju, mundur, "
            "kiri, kanan.",
            PropertyList({
                Property("command", kPropertyTypeString, "stand")
            }),
            [](const PropertyList& properties) -> ReturnValue {
                const std::string command =
                    properties["command"].value<std::string>();

                if (command.empty()) {
                    return "Command is empty.";
                }

                sesameRunCommand(command);

                ESP_LOGI(
                    TAG,
                    "Queued Sesame command: %s",
                    command.c_str()
                );

                return std::string("{\"ok\":true,\"command\":\"") +
                       command + "\"}";
            }
        );
    }

public:
    XiaozhiXSesameBoard()
        : action_button_(ACTION_BUTTON_GPIO) {
        InitializeButton();
        InitializeSesame();

        ESP_LOGI(TAG, "XiaozhiXSesame board initialized");
    }

    AudioCodec* GetAudioCodec() override {
        static NoAudioCodecDuplex audio_codec(
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN
        );

        return &audio_codec;
    }
};

DECLARE_BOARD(XiaozhiXSesameBoard);
