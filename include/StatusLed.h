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
        DatabaseUnavailable,
        WifiConnected,
        Off
    };

    enum class StopIndicator : uint8_t {
        Off,
        Stopping,
        Stopped,
        Killed
    };

    /// Initialisiert die LED-Pins und startet die unabhaengige Anzeigeaufgabe.
    bool begin();

    /// Setzt den anzuzeigenden Zustand threadsicher.
    void setStatus(Status status);

    /// Steuert die separate orange Benutzer-LED unabhaengig von der RGB-LED.
    void setStopIndicator(StopIndicator indicator);

private:
    static void taskEntry(void* parameter);
    void run();
    void writeColor(uint8_t red, uint8_t green, uint8_t blue);

    Status _status = Status::Error;
    StopIndicator _stopIndicator = StopIndicator::Off;
    bool _taskStarted = false;
};
