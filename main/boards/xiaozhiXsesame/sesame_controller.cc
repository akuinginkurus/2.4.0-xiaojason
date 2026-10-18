
#include <driver/i2c_master.h>
#include "sesame_controller.h"
#include "config.h"
#include "face-bitmaps.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include <esp_log.h>
#include <esp_err.h>
#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

static const char* TAG = "XSESAME";

int frameDelay = 350;
int walkCycles = 3;
String currentCommand = "";

static i2c_master_bus_handle_t s_bus = nullptr;
static i2c_master_dev_handle_t s_oled = nullptr;
static i2c_master_dev_handle_t s_pca = nullptr;

static uint8_t s_framebuffer[1024] = {};
static const unsigned char* s_currentBitmap = nullptr;
static FaceAnimMode s_faceMode = FACE_ANIM_ONCE;
static bool s_initialized = false;

static QueueHandle_t s_commandQueue = nullptr;
static TaskHandle_t s_commandTask = nullptr;

static constexpr size_t SESAME_COMMAND_LENGTH = 32;

static void executeSesameCommand(const String& command);

struct FaceEntry {
    const char* name;
    const unsigned char* bitmap;
};

// Menggunakan nama dan bitmap dari FACE_LIST asli.
#define X(name) {#name, epd_bitmap_##name},
static const FaceEntry s_faces[] = {
    FACE_LIST
};
#undef X

static bool writeRegister(uint8_t reg, uint8_t value) {
    if (!s_pca) return false;

    uint8_t data[2] = {reg, value};
    return i2c_master_transmit(s_pca, data, sizeof(data), 100) == ESP_OK;
}

static bool oledCommand(uint8_t command) {
    if (!s_oled) return false;

    uint8_t data[2] = {0x00, command};
    return i2c_master_transmit(s_oled, data, sizeof(data), 100) == ESP_OK;
}

static bool oledWriteData(const uint8_t* data, size_t length) {
    if (!s_oled || !data) return false;

    // Potongan kecil agar transaksi I2C tidak terlalu besar.
    uint8_t packet[17];
    packet[0] = 0x40;

    while (length > 0) {
        size_t count = std::min(length, sizeof(packet) - 1);
        memcpy(packet + 1, data, count);

        if (i2c_master_transmit(
                s_oled, packet, count + 1, 200) != ESP_OK) {
            return false;
        }

        data += count;
        length -= count;
    }

    return true;
}

static bool initOled() {
    const uint8_t initSequence[] = {
        0xAE,       // Display OFF
        0xD5, 0x80,
        0xA8, 0x3F, // Multiplex 1/64
        0xD3, 0x00,
        0x40,
        0x8D, 0x14, // Charge pump
        0x20, 0x02, // Page addressing mode
        0xA1,       // Segment remap
        0xC8,       // COM scan direction
        0xDA, 0x12,
        0x81, 0x7F,
        0xD9, 0xF1,
        0xDB, 0x40,
        0xA4,
        0xA6,
        0xAF        // Display ON
    };

    for (uint8_t command : initSequence) {
        if (!oledCommand(command)) return false;
    }

    memset(s_framebuffer, 0, sizeof(s_framebuffer));

    for (uint8_t page = 0; page < 8; ++page) {
        if (!oledCommand(0xB0 | page)) return false;
        if (!oledCommand(0x00)) return false;
        if (!oledCommand(0x10)) return false;

        if (!oledWriteData(&s_framebuffer[page * 128], 128)) {
            return false;
        }
    }

    return true;
}

static bool initPca9685() {
    // MODE1: auto-increment, oscillator wake/sleep setup.
    if (!writeRegister(0x00, 0x10)) return false;

    // 25 MHz / (4096 * (121 + 1)) = sekitar 50 Hz.
    if (!writeRegister(0xFE, 121)) return false;

    if (!writeRegister(0x01, 0x04)) return false; // MODE2 OUTDRV
    if (!writeRegister(0x00, 0x20)) return false; // Wake + auto increment

    vTaskDelay(pdMS_TO_TICKS(10));

    // Pastikan semua kanal servo awalnya pada posisi tengah.
    for (uint8_t channel = 0; channel < XSESAME_SERVO_COUNT; ++channel) {
        setServoAngle(channel, 90);
    }

    return true;
}

static bool initI2cDevices() {
    i2c_master_bus_config_t busConfig = {};
    busConfig.i2c_port = XSESAME_I2C_PORT;
    busConfig.sda_io_num = XSESAME_I2C_SDA_PIN;
    busConfig.scl_io_num = XSESAME_I2C_SCL_PIN;
    busConfig.clk_source = I2C_CLK_SRC_DEFAULT;
    busConfig.glitch_ignore_cnt = 7;
    busConfig.flags.enable_internal_pullup = true;

    esp_err_t err = i2c_new_master_bus(&busConfig, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Gagal membuat bus I2C: %s", esp_err_to_name(err));
        return false;
    }

    i2c_device_config_t oledConfig = {};
    oledConfig.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    oledConfig.device_address = XSESAME_OLED_I2C_ADDRESS;
    oledConfig.scl_speed_hz = XSESAME_I2C_FREQ_HZ;

    err = i2c_master_bus_add_device(s_bus, &oledConfig, &s_oled);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OLED tidak dapat ditambahkan: %s", esp_err_to_name(err));
        return false;
    }

    i2c_device_config_t pcaConfig = {};
    pcaConfig.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    pcaConfig.device_address = XSESAME_PCA9685_ADDRESS;
    pcaConfig.scl_speed_hz = XSESAME_I2C_FREQ_HZ;

    err = i2c_master_bus_add_device(s_bus, &pcaConfig, &s_pca);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCA9685 tidak dapat ditambahkan: %s", esp_err_to_name(err));
        return false;
    }

    return true;
}

bool sesameControllerInit() {
    if (s_initialized) return true;

    if (!initI2cDevices()) return false;

    if (!initOled()) {
        ESP_LOGE(TAG, "Inisialisasi SSD1306 gagal");
        return false;
    }

    if (!initPca9685()) {
        ESP_LOGE(TAG, "Inisialisasi PCA9685 gagal");
        return false;
    }

    s_initialized = true;
    setFace("idle");

    ESP_LOGI(TAG, "OLED SSD1306 dan PCA9685 siap");
    return true;
}

void setServoAngle(uint8_t channel, int angle) {
    if (!s_pca || channel >= XSESAME_SERVO_COUNT) return;

    angle = std::max(0, std::min(180, angle));

    const uint32_t pulseMin =
        (XSESAME_SERVO_MIN_PULSE_US * 4096UL * XSESAME_SERVO_FREQUENCY) / 1000000UL;
    const uint32_t pulseMax =
        (XSESAME_SERVO_MAX_PULSE_US * 4096UL * XSESAME_SERVO_FREQUENCY) / 1000000UL;

    uint32_t pulse = pulseMin +
        ((pulseMax - pulseMin) * static_cast<uint32_t>(angle)) / 180UL;

    pulse = std::min<uint32_t>(pulse, 4095);

    uint8_t reg = 0x06 + (channel * 4);
    writeRegister(reg, 0);                       // ON low
    writeRegister(reg + 1, 0);                   // ON high
    writeRegister(reg + 2, pulse & 0xFF);        // OFF low
    writeRegister(reg + 3, (pulse >> 8) & 0x0F); // OFF high
}

void setFace(const String& faceName) {
    if (!s_oled) return;

    const unsigned char* bitmap = nullptr;

    for (const auto& face : s_faces) {
        if (faceName == face.name && face.bitmap != nullptr) {
            bitmap = face.bitmap;
            break;
        }
    }

    // Fallback bila bitmap dengan nama yang diminta tidak tersedia.
    if (!bitmap) {
        for (const auto& face : s_faces) {
            if (strcmp(face.name, "idle") == 0 && face.bitmap != nullptr) {
                bitmap = face.bitmap;
                break;
            }
        }
    }

    if (!bitmap) {
        ESP_LOGW(TAG, "Bitmap wajah tidak tersedia: %s", faceName.c_str());
        return;
    }

    s_currentBitmap = bitmap;

    // Bitmap image2cpp 128x64 monochrome, 1 bit per pixel.
    memcpy(s_framebuffer, s_currentBitmap, sizeof(s_framebuffer));

    for (uint8_t page = 0; page < 8; ++page) {
        oledCommand(0xB0 | page);
        oledCommand(0x00);
        oledCommand(0x10);
        oledWriteData(&s_framebuffer[page * 128], 128);
    }
}

void setFaceMode(FaceAnimMode mode) {
    s_faceMode = mode;
}

void setFaceWithMode(const String& faceName, FaceAnimMode mode) {
    setFaceMode(mode);
    setFace(faceName);
}

void delayWithFace(unsigned long ms) {
    // Potong jeda menjadi interval pendek supaya task lain tetap berjalan.
    // Bitmap statis tetap tampil selama gerakan berlangsung.
    while (ms > 0) {
        const unsigned long slice = std::min<unsigned long>(ms, 20);
        vTaskDelay(pdMS_TO_TICKS(slice));
        ms -= slice;
    }
}

void enterIdle() {
    setFaceWithMode("idle", FACE_ANIM_LOOP);
}

bool pressingCheck(String cmd, int ms) {
    if (currentCommand != cmd) return false;

    if (ms > 0) {
        delayWithFace(static_cast<unsigned long>(ms));
    }

    return currentCommand == cmd;
}

void sesameStopMovement() {
    currentCommand = "";
}

static String normalizeCommand(String command) {
    std::transform(command.begin(), command.end(), command.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return command;
}

static void executeSesameCommand(const String& requestedCommand) {

void sesameRunCommand(const String& command) {
    if (!s_initialized || s_commandQueue == nullptr) {
        ESP_LOGW(TAG, "Controller belum siap");
        return;
    }

    char buffer[SESAME_COMMAND_LENGTH] = {};
    size_t length = command.size();

    if (length >= sizeof(buffer)) {
        length = sizeof(buffer) - 1;
    }

    memcpy(buffer, command.data(), length);

    if (xQueueSend(s_commandQueue, buffer, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Antrean penuh; perintah tidak dikirim");
    }
}
    
    if (!s_initialized) {
        ESP_LOGE(TAG, "Controller belum diinisialisasi");
        return;
    }

    String command = normalizeCommand(requestedCommand);
    currentCommand = command;

    if (command == "rest" || command == "istirahat") {
        runRestPose();
    } else if (command == "stand" || command == "berdiri") {
        runStandPose(1);
    } else if (command == "wave" || command == "melambai") {
        runWavePose();
    } else if (command == "dance" || command == "menari") {
        runDancePose();
    } else if (command == "swim" || command == "berenang") {
        runSwimPose();
    } else if (command == "point" || command == "menunjuk") {
        runPointPose();
    } else if (command == "pushup" || command == "push up") {
        runPushupPose();
    } else if (command == "bow" || command == "membungkuk") {
        runBowPose();
    } else if (command == "cute" || command == "imut") {
        runCutePose();
    } else if (command == "freaky") {
        runFreakyPose();
    } else if (command == "worm") {
        runWormPose();
    } else if (command == "shake" || command == "goyang") {
        runShakePose();
    } else if (command == "shrug") {
        runShrugPose();
    } else if (command == "dead" || command == "pura-pura mati") {
        runDeadPose();
    } else if (command == "crab" || command == "kepiting") {
        runCrabPose();
    } else if (command == "forward" || command == "maju") {
        runWalkPose();
    } else if (command == "backward" || command == "mundur") {
        runWalkBackward();
    } else if (command == "left" || command == "kiri") {
        runTurnLeft();
    } else if (command == "right" || command == "kanan") {
        runTurnRight();
    } else if (command == "idle") {
        enterIdle();
        currentCommand = "";
    } else {
        ESP_LOGW(TAG, "Perintah gerakan tidak dikenal: %s", command.c_str());
        currentCommand = "";
    }

    currentCommand = "";
}
