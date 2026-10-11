#pragma once

#include <Arduino.h>

/**
 * @brief Zeigt den aktuellen Betriebszustand ueber die Nano-ESP32-RGB-LED.
 */
class StatusLed {
public:
    enum class Status : uint8_t {
        Error,
        Uploading,
        StorageCritical,
        WifiConnected
    };

    /// Initialisiert die LED-Pins und startet die unabhaengige Anzeigeaufgabe.
    bool begin();

    /// Setzt den anzuzeigenden Zustand threadsicher.
    void setStatus(Status status);

private:
    static void taskEntry(void* parameter);
    void run();
    void writeColor(uint8_t red, uint8_t green, uint8_t blue);

    Status _status = Status::Error;
    bool _taskStarted = false;
};
