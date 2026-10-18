

#pragma once

#include <cstdint>
#include <string>
#include <cstdio>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

#ifndef PROGMEM
#define PROGMEM
#endif

#ifndef F
#define F(x) x
#endif

using String = std::string;

class SesameSerialCompat {
public:
    void begin(unsigned long) {}

    template<typename T>
    void print(const T& value) {
        printf("%s", toText(value));
    }

    template<typename T>
    void println(const T& value) {
        printf("%s\n", toText(value));
    }

    void println() {
        printf("\n");
    }

private:
    static const char* toText(const std::string& s) {
        return s.c_str();
    }

    static const char* toText(const char* s) {
        return s ? s : "";
    }

    static const char* toText(char* s) {
        return s ? s : "";
    }

    template<typename T>
    static const char* toText(const T& value) {
        static char buffer[32];
        snprintf(buffer, sizeof(buffer), "%ld",
                 static_cast<long>(value));
        return buffer;
    }
};

inline SesameSerialCompat Serial;

inline void delay(unsigned long ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}
