#pragma once

#include <Arduino.h>
#include <time.h>

/**
 * @brief Richtet die ESP32-Systemuhr fuer NTP und lokale Zeit ein.
 */
class NtpClock {
public:
    /**
     * @brief Startet SNTP einmalig mit den zentralen Zeitzonen- und Serverwerten.
     */
    void begin();

    /**
     * @brief Liefert eine plausible, vom NTP-Client gesetzte Unix-Zeit.
     * @param currentTime Zielvariable fuer den Zeitstempel.
     * @return true, wenn die Systemuhr synchronisiert beziehungsweise plausibel ist.
     */
    bool getTime(time_t& currentTime) const;

    /**
     * @brief Protokolliert die erste erkannte NTP-Synchronisation.
     */
    void logStatus();

private:
    /// Verhindert mehrfache NTP-Konfiguration bei WLAN-Reconnects.
    bool _started = false;

    /// Verhindert wiederholte Erfolgsmeldungen in jeder Hauptschleife.
    bool _synchronizationLogged = false;
};
