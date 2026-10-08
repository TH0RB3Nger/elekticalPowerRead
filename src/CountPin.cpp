#include "CountPin.h"

/**
 * @brief Zaehlt einen gueltigen Impuls, wenn die Entprellzeit abgelaufen ist.
 * @param arg Zeiger auf die CountPin-Instanz.
 */
void IRAM_ATTR CountPin::handleInterrupt(void* arg) {
    CountPin* counter = static_cast<CountPin*>(arg);
    const unsigned long now = micros();

    if (now - counter->_lastInterruptTime >= counter->_debounceDelay * 1000UL) {
        counter->_countValue++;
        counter->_lastInterruptTime = now;
    }
}

/**
 * @brief Initialisiert den Pin und registriert den steigenden Interrupt.
 */
CountPin::CountPin(int pin, __long_double_t factor, unsigned long debounceDelay)
    : _pin(pin),
      _factor(factor),
      _countValue(0),
      _lastInterruptTime(0),
      _debounceDelay(debounceDelay) {
    pinMode(_pin, INPUT_PULLDOWN);
    attachInterruptArg(
        digitalPinToInterrupt(_pin),
        handleInterrupt,
        this,
        RISING
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
 * @brief Berechnet den skalierten Gesamtwert aus einem konsistenten Stand.
 */
__long_double_t CountPin::getValue() const {
    noInterrupts();
    const __long_double_t value = _countValue * _factor;
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
