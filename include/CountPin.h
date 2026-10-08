#pragma once

#include <Arduino.h>
#include <driver/gpio.h>

/**
 * @brief Zaehler fuer Pulse, die wegen Dauer oder Mindestabstand verworfen wurden.
 */
struct RejectedPulseCounts {
    uint64_t tooShort;
    uint64_t tooSoon;
    bool tooShortSaturated;
    bool tooSoonSaturated;
    bool countSaturated;
};

class CountPin {
private:
    /// GPIO, Zaehleraufloesung und interruptgeschuetzter Impulszustand.
    int _pin;
    const double _resolutionPulsesPerKWh;
    const bool _pulseActiveHigh;
    volatile uint64_t _countValue;
    volatile uint32_t _pulseStartTime;
    volatile bool _pulseActive;
    volatile uint32_t _lastPulseStartTime;
    volatile bool _hasLastPulseStart;
    volatile uint64_t _tooShortPulseCount;
    volatile uint64_t _tooSoonPulseCount;
    volatile bool _tooShortPulseCountSaturated;
    volatile bool _tooSoonPulseCountSaturated;
    volatile bool _countSaturated;
    const uint64_t _minimumPulseDurationUs;
    const uint64_t _minimumPulseIntervalUs;

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
    uint64_t getCount() const;

    /**
     * @brief Entfernt einen bereits dauerhaft gepufferten Teilstand atomar.
     * @return false, wenn der Zaehler seit dem Snapshot gesaettigt wurde.
     */
    bool subtractCount(uint64_t count);

    /// Gibt den Zaehlerstand in kWh gemaess der konfigurierten Aufloesung zurueck.
    double getValue() const;

    /// Liest ISR-Diagnosezaehler atomar; gibt keine Logs aus der Interrupt-Routine aus.
    RejectedPulseCounts getRejectedPulseCounts() const;

    /// Setzt den Impulsstand interruptgeschuetzt auf null.
    void reset();
};
