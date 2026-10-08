#include "CountPin.h"

/// Verarbeitet beide Flanken, um aktive Pulsdauer und Intervall zu pruefen.
void IRAM_ATTR CountPin::handleInterrupt(void* arg) {
    CountPin* counter = static_cast<CountPin*>(arg);
    const unsigned long now = micros();
    const bool levelIsHigh =
        gpio_get_level(static_cast<gpio_num_t>(counter->_pin)) != 0;
    const bool pulseIsActive =
        counter->_pulseActiveHigh ? levelIsHigh : !levelIsHigh;

    if (pulseIsActive) {
        counter->_pulseStartTime = now;
        counter->_pulseActive = true;
        return;
    }

    const unsigned long pulseStart = counter->_pulseStartTime;
    if (!counter->_pulseActive) {
        return;
    }

    counter->_pulseActive = false;
    counter->_pulseStartTime = 0;
    if (now - pulseStart < counter->_minimumPulseDurationUs) {
        return;
    }

    const unsigned long lastPulseStart = counter->_lastPulseStartTime;
    if (lastPulseStart != 0
        && pulseStart - lastPulseStart < counter->_minimumPulseIntervalUs) {
        return;
    }

    counter->_countValue++;
    counter->_lastPulseStartTime = pulseStart;
}

/**
 * @brief Initialisiert den Pin und registriert beide S0-Signalflanken.
 */
CountPin::CountPin(
    int pin,
    double resolutionPulsesPerKWh,
    bool pulseActiveHigh,
    unsigned long minimumPulseDurationMs,
    unsigned long minimumPulseIntervalMs
)
    : _pin(pin),
      _resolutionPulsesPerKWh(resolutionPulsesPerKWh),
      _pulseActiveHigh(pulseActiveHigh),
      _countValue(0),
      _pulseStartTime(0),
      _pulseActive(false),
      _lastPulseStartTime(0),
      _minimumPulseDurationUs(minimumPulseDurationMs * 1000UL),
      _minimumPulseIntervalUs(minimumPulseIntervalMs * 1000UL) {
    pinMode(_pin, _pulseActiveHigh ? INPUT_PULLDOWN : INPUT_PULLUP);
    attachInterruptArg(
        digitalPinToInterrupt(_pin),
        handleInterrupt,
        this,
        CHANGE
    );
}

/**
 * @brief Liest den Impulsstand ohne gleichzeitige ISR-Aenderung.
 */
unsigned long CountPin::getCount() const {
    noInterrupts();
    const unsigned long value = _countValue;
    interrupts();
    return value;
}

/**
 * @brief Berechnet den kWh-Gesamtwert aus einem konsistenten Impulsstand.
 */
double CountPin::getValue() const {
    noInterrupts();
    const double value = _countValue / _resolutionPulsesPerKWh;
    interrupts();
    return value;
}

/**
 * @brief Setzt den gespeicherten Impulsstand interruptgeschuetzt zurueck.
 */
void CountPin::reset() {
    noInterrupts();
    _countValue = 0;
    interrupts();
}
