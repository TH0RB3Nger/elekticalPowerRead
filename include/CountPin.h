#pragma once

#include <Arduino.h>
#include <driver/gpio.h>

class CountPin {
private:
    /// GPIO, Zaehleraufloesung und interruptgeschuetzter Impulszustand.
    int _pin;
    const double _resolutionPulsesPerKWh;
    const bool _pulseActiveHigh;
    volatile unsigned long _countValue;
    volatile unsigned long _pulseStartTime;
    volatile bool _pulseActive;
    volatile unsigned long _lastPulseStartTime;
    const unsigned long _minimumPulseDurationUs;
    const unsigned long _minimumPulseIntervalUs;

    /// Statischer ISR-Einstiegspunkt fuer attachInterruptArg.
    static void IRAM_ATTR handleInterrupt(void* arg);

public:
    /**
     * @brief Konfiguriert einen GPIO als entprellten Impulszaehler.
     * @param pin GPIO-Nummer.
     * @param resolutionPulsesPerKWh Zaehleraufloesung in Impulsen pro kWh.
     * @param pulseActiveHigh true, wenn der Impuls bei HIGH aktiv ist.
     * @param minimumPulseDurationMs Mindestdauer des aktiven Pegels.
     * @param minimumPulseIntervalMs Mindestabstand zwischen gezaehlten Impulsen.
     */
    CountPin(
        int pin,
        double resolutionPulsesPerKWh,
        bool pulseActiveHigh,
        unsigned long minimumPulseDurationMs,
        unsigned long minimumPulseIntervalMs
    );

    /// Gibt den atomar gelesenen Impulsstand zurueck.
    unsigned long getCount() const;

    /// Gibt den Zaehlerstand in kWh gemaess der konfigurierten Aufloesung zurueck.
    double getValue() const;

    /// Setzt den Impulsstand interruptgeschuetzt auf null.
    void reset();
};
