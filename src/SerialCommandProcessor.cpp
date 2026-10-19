#include "SerialCommandProcessor.h"

#include <cctype>
#include <cstring>

/// Startet einen kleinen Hintergrundtask fuer sofortige Stop-Befehle.
bool SerialCommandProcessor::begin(
    ImmediateStopCallback callback,
    void* context
) {
    if (_queue != nullptr) {
        return true;
    }
    _queue = xQueueCreate(8, sizeof(SerialCommand));
    if (_queue == nullptr) {
        return false;
    }
    _callback = callback;
    _callbackContext = context;
    if (xTaskCreate(taskEntry, "serial_commands", 3072, this, 1, nullptr)
        != pdPASS) {
        vQueueDelete(_queue);
        _queue = nullptr;
        return false;
    }
    return true;
}

/// Uebergibt den naechsten Hintergrundbefehl oder liest bei Taskfehler selbst.
bool SerialCommandProcessor::poll(SerialCommand& command) {
    if (_queue != nullptr) {
        return xQueueReceive(_queue, &command, 0) == pdPASS;
    }
    return readNextCommand(command);
}

/// Leitet den FreeRTOS-Einstieg an die Instanz weiter.
void SerialCommandProcessor::taskEntry(void* parameter) {
    static_cast<SerialCommandProcessor*>(parameter)->run();
}

/// Liest Befehle unabhaengig von synchronen Netzwerkoperationen ein.
void SerialCommandProcessor::run() {
    bool wasConnected = false;
    while (true) {
        const bool connected = static_cast<bool>(Serial);
        if (connected && !wasConnected) {
            Serial.println("USB-Serial bereit. HELP oder ? fuer Befehle.");
        }
        wasConnected = connected;

        SerialCommand command;
        if (readNextCommand(command)) {
            if (command.type == SerialCommandType::Help) {
                Serial.println("Befehle:");
                Serial.println("  STOP                  Erfassung pausieren und Puffer uebertragen");
                Serial.println("  START                 Erfassung wieder starten");
                Serial.println("  KILL                  Sofort stoppen, ohne Puffer-Upload");
                Serial.println("  DEBUG <LEVEL>         ERROR|WARNING|INFO|DEBUG|DEBUG_PLUS");
                Serial.println("  HELP oder ?           Diese Hilfe anzeigen");
                continue;
            }
            if ((command.type == SerialCommandType::Stop
                    || command.type == SerialCommandType::Kill)
                && _callback != nullptr) {
                _callback(command.type, _callbackContext);
                Serial.println(command.type == SerialCommandType::Kill
                    ? "KILL empfangen: Zaehl-Interrupts sofort pausiert."
                    : "STOP empfangen: Zaehl-Interrupts sofort pausiert.");
            }
            xQueueSend(_queue, &command, portMAX_DELAY);
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

/// Sammelt serielle Zeichen ohne blockierendes Warten auf eine Zeile.
bool SerialCommandProcessor::readNextCommand(SerialCommand& command) {
    while (Serial.available() > 0) {
        const char input = static_cast<char>(Serial.read());
        if (input == '\r') {
            continue;
        }
        if (input == '\n') {
            if (_length == 0 && !_overflow) {
                continue;
            }
            _buffer[_length] = '\0';
            const bool parsed = parseCommand(command);
            _length = 0;
            _overflow = false;
            return parsed;
        }
        if (_length + 1 < sizeof(_buffer)) {
            _buffer[_length++] = input;
        } else {
            _overflow = true;
        }
    }
    return false;
}

/// Ordnet bekannte Befehle und Logstufen zu, ohne dynamische Strings anzulegen.
bool SerialCommandProcessor::parseCommand(SerialCommand& command) {
    command = {};
    if (_overflow) {
        return true;
    }

    size_t start = 0;
    while (_buffer[start] == ' ' || _buffer[start] == '\t') {
        ++start;
    }
    if (start == _length) {
        return false;
    }

    char* text = _buffer + start;
    for (char* character = text; *character != '\0'; ++character) {
        *character = static_cast<char>(toupper(static_cast<unsigned char>(*character)));
    }
    size_t end = strlen(text);
    while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
        text[--end] = '\0';
    }

    if (strcmp(text, "?") == 0 || strcmp(text, "HELP") == 0) {
        command.type = SerialCommandType::Help;
    } else if (strcmp(text, "STOP") == 0) {
        command.type = SerialCommandType::Stop;
    } else if (strcmp(text, "START") == 0) {
        command.type = SerialCommandType::Start;
    } else if (strcmp(text, "KILL") == 0) {
        command.type = SerialCommandType::Kill;
    } else if (strncmp(text, "DEBUG", 5) == 0
        && (text[5] == ' ' || text[5] == '\t')) {
        char* level = text + 6;
        while (*level == ' ' || *level == '\t') {
            ++level;
        }
        if (strcmp(level, "ERROR") == 0) {
            command.logLevel = LOG_ERROR;
        } else if (strcmp(level, "WARNING") == 0 || strcmp(level, "WARN") == 0) {
            command.logLevel = LOG_WARNING;
        } else if (strcmp(level, "INFO") == 0) {
            command.logLevel = LOG_INFO;
        } else if (strcmp(level, "DEBUG") == 0) {
            command.logLevel = LOG_DEBUG;
        } else if (strcmp(level, "DEBUG_PLUS") == 0 || strcmp(level, "DEBUG+") == 0) {
            command.logLevel = LOG_DEBUG_PLUS;
        } else {
            command.type = SerialCommandType::Invalid;
            return true;
        }
        command.type = SerialCommandType::SetLogLevel;
    } else {
        command.type = SerialCommandType::Invalid;
    }
    return true;
}