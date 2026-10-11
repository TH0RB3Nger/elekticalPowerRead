#include "StatusLed.h"

#include <freertos/FreeRTOS.h>

#include "Logging.h"

namespace {
portMUX_TYPE statusMux = portMUX_INITIALIZER_UNLOCKED;
constexpr uint8_t ledBrightness = 48;
constexpr TickType_t ledTaskInterval = pdMS_TO_TICKS(50);
constexpr TickType_t uploadBlinkHalfPeriod = pdMS_TO_TICKS(500);
}

bool StatusLed::begin() {
    pinMode(LED_RED, OUTPUT);
    pinMode(LED_GREEN, OUTPUT);
    pinMode(LED_BLUE, OUTPUT);
    writeColor(0, 0, 0);

    if (_taskStarted) {
        return true;
    }

    if (xTaskCreate(
            taskEntry,
            "status_led",
            3072,
            this,
            1,
            nullptr) != pdPASS) {
        writeColor(ledBrightness, 0, 0);
        logError("Status-LED-Aufgabe konnte nicht gestartet werden");
        return false;
    }

    _taskStarted = true;
    return true;
}

void StatusLed::setStatus(Status status) {
    portENTER_CRITICAL(&statusMux);
    _status = status;
    portEXIT_CRITICAL(&statusMux);
}

void StatusLed::taskEntry(void* parameter) {
    static_cast<StatusLed*>(parameter)->run();
}

void StatusLed::run() {
    Status previousStatus = Status::Error;
    bool previousBlinkPhase = false;
    bool hasRendered = false;

    while (true) {
        Status status;
        portENTER_CRITICAL(&statusMux);
        status = _status;
        portEXIT_CRITICAL(&statusMux);

        const bool blinkPhase =
            ((xTaskGetTickCount() / uploadBlinkHalfPeriod) % 2) == 0;
        if (!hasRendered
            || status != previousStatus
            || (status == Status::Uploading
                && blinkPhase != previousBlinkPhase)) {
            switch (status) {
                case Status::Error:
                    writeColor(ledBrightness, 0, 0);
                    break;
                case Status::Uploading:
                    writeColor(
                        blinkPhase ? ledBrightness : 0,
                        blinkPhase ? ledBrightness : 0,
                        0
                    );
                    break;
                case Status::StorageCritical:
                    writeColor(ledBrightness, 12, 0);
                    break;
                case Status::WifiConnected:
                    writeColor(0, ledBrightness, 0);
                    break;
            }

            previousStatus = status;
            previousBlinkPhase = blinkPhase;
            hasRendered = true;
        }

        vTaskDelay(ledTaskInterval);
    }
}

void StatusLed::writeColor(
    uint8_t red,
    uint8_t green,
    uint8_t blue
) {
    analogWrite(LED_RED, 255 - red);
    analogWrite(LED_GREEN, 255 - green);
    analogWrite(LED_BLUE, 255 - blue);
}
