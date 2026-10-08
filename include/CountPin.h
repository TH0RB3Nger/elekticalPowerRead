#pragma once

#include <Arduino.h>

class CountPin {
private:
    /// GPIO, Umrechnungsfaktor und interruptgeschuetzter Impulszustand.
    int _pin;
    const __long_double_t _factor;
    volatile unsigned long _countValue;
    volatile unsigned long _lastInterruptTime;
    unsigned long _debounceDelay;

    /// Statischer ISR-Einstiegspunkt fuer attachInterruptArg.
    static void IRAM_ATTR handleInterrupt(void* arg);

public:
    /**
     * @brief Konfiguriert einen GPIO als entprellten Impulszaehler.
     * @param pin GPIO-Nummer.
     * @param factor Multiplikator fuer die Umrechnung des Impulsstands.
     * @param debounceDelay Entprellzeit in Millisekunden.
     */
    CountPin(int pin, __long_double_t factor, unsigned long debounceDelay);

    /// Gibt den atomar gelesenen Impulsstand zurueck.
    unsigned long getCount() const;

    /// Gibt Impulsstand mal Umrechnungsfaktor zurueck.
    __long_double_t getValue() const;

    /// Setzt den Impulsstand interruptgeschuetzt auf null.
    void reset();
};
