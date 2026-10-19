#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "Logging.h"

enum class SerialCommandType : uint8_t {
    Help,
    Stop,
    Start,
    Kill,
    SetLogLevel,
    Invalid
};

struct SerialCommand {
    SerialCommandType type = SerialCommandType::Invalid;
    LogLevel logLevel = LOG_INFO;
};

/**
 * @brief Liest zeilenweise Befehle mit einem festen Eingabepuffer von Serial.
 */
class SerialCommandProcessor {
public:
    using ImmediateStopCallback = void (*)(SerialCommandType type, void* context);

    /// Startet den Empfangstask; der Callback pausiert die GPIO-Erfassung sofort.
    bool begin(ImmediateStopCallback callback, void* context);

    /// Liefert genau dann einen Befehl, wenn eine vollstaendige Zeile vorliegt.
    bool poll(SerialCommand& command);

private:
    static void taskEntry(void* parameter);
    void run();
    bool readNextCommand(SerialCommand& command);
    bool parseCommand(SerialCommand& command);

    QueueHandle_t _queue = nullptr;
    ImmediateStopCallback _callback = nullptr;
    void* _callbackContext = nullptr;
    char _buffer[40]{};
    size_t _length = 0;
    bool _overflow = false;
};