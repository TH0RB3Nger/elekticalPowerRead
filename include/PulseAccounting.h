#pragma once

#include <Arduino.h>

#include "CountPin.h"
#include "ConsumptionBuffer.h"

/**
 * @brief Uebernimmt neue GPIO-Impulse genau einmal in den Verbrauchspuffer.
 *
 * Die Klasse behaelt je Kanal den bereits uebernommenen Impulsstand und
 * protokolliert ISR-Diagnosen im Haupttask statt direkt aus einer ISR.
 */
class PulseAccounting {
public:
    PulseAccounting(
        ConsumptionBuffer& buffer,
        CountPin& counter1,
        uint8_t counterId1,
        CountPin& counter2,
        uint8_t counterId2,
        CountPin& counter3,
        uint8_t counterId3,
        CountPin& counter4,
        uint8_t counterId4
    );

    /// Uebernimmt neue Impulse und aktualisiert die Diagnosemeldungen.
    void process(bool bufferInitialized);

    /// Uebernimmt nach dem Pausieren der GPIOs abschliessend alle Pulse.
    void flush(bool bufferInitialized);

private:
    struct Channel {
        CountPin* counter;
        uint8_t counterId;
    };

    void bufferNewPulses(size_t index);
    void logRejectedPulseDiagnostics(size_t index);

    ConsumptionBuffer& _buffer;
    Channel _channels[4];
    uint64_t _bufferedCounts[4]{};
    RejectedPulseCounts _lastRejected[4]{};
};
