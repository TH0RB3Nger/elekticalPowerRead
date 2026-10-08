#include "CountPin.h"

namespace {
/// Inkrementiert ISR-Zaehler ohne Wraparound.
void IRAM_ATTR incrementSaturating(
    volatile uint64_t& value,
    volatile bool& saturated
) {
    if (value == UINT64_MAX) {
        saturated = true;
        return;
    }
    ++value;
}

/// Wandelt die konfigurierten Millisekunden ohne Multiplikationsueberlauf um.
uint64_t millisecondsToMicroseconds(unsigned long milliseconds) {
    return static_cast<uint64_t>(milliseconds) * 1000ULL;
}
}

/// Verarbeitet beide Flanken, um aktive Pulsdauer und Intervall zu pruefen.
void IRAM_ATTR CountPin::handleInterrupt(void* arg) {
    CountPin* counter = static_cast<CountPin*>(arg);
    const uint32_t now = micros();
    const bool levelIsHigh =
        gpio_get_level(static_cast<gpio_num_t>(counter->_pin)) != 0;
    const bool pulseIsActive =
        counter->_pulseActiveHigh ? levelIsHigh : !levelIsHigh;

    if (pulseIsActive) {
        counter->_pulseStartTime = now;
        counter->_pulseActive = true;
        return;
    }

    const uint32_t pulseStart = counter->_pulseStartTime;
    if (!counter->_pulseActive) {
        return;
    }

    counter->_pulseActive = false;
    counter->_pulseStartTime = 0;
    if (static_cast<uint32_t>(now - pulseStart)
        < counter->_minimumPulseDurationUs) {
        incrementSaturating(
            counter->_tooShortPulseCount,
            counter->_tooShortPulseCountSaturated
        );
        return;
    }

    const uint32_t lastPulseStart = counter->_lastPulseStartTime;
    if (counter->_hasLastPulseStart
        && static_cast<uint32_t>(pulseStart - lastPulseStart)
            < counter->_minimumPulseIntervalUs) {
        incrementSaturating(
            counter->_tooSoonPulseCount,
            counter->_tooSoonPulseCountSaturated
        );
        return;
    }

    if (counter->_countValue == UINT64_MAX) {
        counter->_countSaturated = true;
        return;
    }
    ++counter->_countValue;
    counter->_lastPulseStartTime = pulseStart;
    counter->_hasLastPulseStart = true;
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
      _hasLastPulseStart(false),
      _tooShortPulseCount(0),
      _tooSoonPulseCount(0),
      _tooShortPulseCountSaturated(false),
      _tooSoonPulseCountSaturated(false),
      _countSaturated(false),
      _minimumPulseDurationUs(millisecondsToMicroseconds(minimumPulseDurationMs)),
      _minimumPulseIntervalUs(millisecondsToMicroseconds(minimumPulseIntervalMs)) {
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
uint64_t CountPin::getCount() const {
    noInterrupts();
    const uint64_t value = _countValue;
    interrupts();
    return value;
}

bool CountPin::subtractCount(uint64_t count) {
    noInterrupts();
    if (_countSaturated || count > _countValue) {
        interrupts();
        return false;
    }

    _countValue -= count;
    interrupts();
    return true;
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

RejectedPulseCounts CountPin::getRejectedPulseCounts() const {
    noInterrupts();
    const RejectedPulseCounts counts{
        _tooShortPulseCount,
        _tooSoonPulseCount,
        _tooShortPulseCountSaturated,
        _tooSoonPulseCountSaturated,
        _countSaturated
    };
    interrupts();
    return counts;
}

/**
 * @brief Setzt den gespeicherten Impulsstand interruptgeschuetzt zurueck.
 */
void CountPin::reset() {
    noInterrupts();
    _countValue = 0;
    _countSaturated = false;
    interrupts();
}
