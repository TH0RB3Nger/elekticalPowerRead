#include "PulseAccounting.h"

#include <cmath>

#include "Config.h"
#include "Logging.h"

namespace {
constexpr size_t counterCount = 4;
constexpr uint64_t rolloverThreshold = UINT64_MAX - 1000000ULL;
}

PulseAccounting::PulseAccounting(
    ConsumptionBuffer& buffer,
    CountPin& counter1,
    uint8_t counterId1,
    CountPin& counter2,
    uint8_t counterId2,
    CountPin& counter3,
    uint8_t counterId3,
    CountPin& counter4,
    uint8_t counterId4
)
    : _buffer(buffer),
      _channels{
          {&counter1, counterId1},
          {&counter2, counterId2},
          {&counter3, counterId3},
          {&counter4, counterId4}
      } {}

/// Verarbeitet Zaehlerdifferenzen und Diagnosewerte ausserhalb der ISR.
void PulseAccounting::process(bool bufferInitialized) {
    if (!bufferInitialized) {
        return;
    }

    for (size_t index = 0; index < counterCount; ++index) {
        bufferNewPulses(index);
        logRejectedPulseDiagnostics(index);
    }
}

/// Sichert den letzten Zaehlstand, nachdem die Interrupts pausiert wurden.
void PulseAccounting::flush(bool bufferInitialized) {
    if (!bufferInitialized) {
        return;
    }

    for (size_t index = 0; index < counterCount; ++index) {
        bufferNewPulses(index);
    }
}

/// Addiert nur den Anteil seit dem letzten erfolgreichen Puffertransfer.
void PulseAccounting::bufferNewPulses(size_t index) {
    Channel& channel = _channels[index];
    const uint64_t currentCount = channel.counter->getCount();
    uint64_t& bufferedCount = _bufferedCounts[index];

    if (currentCount < bufferedCount) {
        logError(
            "Impulszaehler fuer ID " + String(channel.counterId)
            + " wurde zurueckgesetzt; passe den gespeicherten Stand an"
        );
        bufferedCount = currentCount;
        return;
    }

    if (currentCount != bufferedCount) {
        const uint64_t newPulses = currentCount - bufferedCount;
        const double addedConsumption =
            newPulses / Config::Runtime::resolutionPulsesPerKWh;
        if (!isfinite(addedConsumption)) {
            logError(
                "Verbrauchsdifferenz fuer Zaehler-ID "
                + String(channel.counterId)
                + " ist ausserhalb des darstellbaren Zahlenbereichs"
            );
            return;
        }

        logDebug(
            "Neue gueltige Pulse fuer Zaehler-ID " + String(channel.counterId)
            + ": " + String(static_cast<unsigned long long>(newPulses))
            + " (" + String(addedConsumption, 6) + " kWh)"
        );
        if (!_buffer.addConsumption(channel.counterId, addedConsumption)) {
            return;
        }
        bufferedCount = currentCount;
        logDebug(
            "Verbrauch fuer Zaehler-ID " + String(channel.counterId)
            + " im konfigurierten Puffer uebernommen"
        );
    }

    if (currentCount < rolloverThreshold || bufferedCount != currentCount) {
        return;
    }
    if (!_buffer.requestImmediateUpload()) {
        return;
    }
    if (!channel.counter->subtractCount(currentCount)) {
        logError(
            "Impulszaehler fuer ID " + String(channel.counterId)
            + " konnte wegen einer Saettigung nicht sicher entlastet werden"
        );
        return;
    }

    bufferedCount = 0;
    logWarning(
        "Impulszaehler fuer ID " + String(channel.counterId)
        + " nahe am Zahlenlimit; gepufferter Stand wird sofort uebertragen"
    );
}

/// Meldet Diagnosezaehler ausserhalb der ISR und nur bei neuem Zustand.
void PulseAccounting::logRejectedPulseDiagnostics(size_t index) {
    const Channel& channel = _channels[index];
    RejectedPulseCounts& lastCounts = _lastRejected[index];
    const RejectedPulseCounts counts = channel.counter->getRejectedPulseCounts();
    if (counts.tooShort == lastCounts.tooShort
        && counts.tooSoon == lastCounts.tooSoon
        && counts.tooShortSaturated == lastCounts.tooShortSaturated
        && counts.tooSoonSaturated == lastCounts.tooSoonSaturated
        && counts.countSaturated == lastCounts.countSaturated) {
        return;
    }

    logDebugPlus(
        "ISR-Diagnose Zaehler-ID " + String(channel.counterId)
        + ": zu kurze Pulse +" + String(counts.tooShort - lastCounts.tooShort)
        + ", Mindestabstand verletzt +"
        + String(counts.tooSoon - lastCounts.tooSoon)
    );
    if (counts.tooShortSaturated && !lastCounts.tooShortSaturated) {
        logError("ISR-Diagnosezaehler fuer zu kurze Pulse ist ausgeschoepft");
    }
    if (counts.tooSoonSaturated && !lastCounts.tooSoonSaturated) {
        logError("ISR-Diagnosezaehler fuer zu dichte Pulse ist ausgeschoepft");
    }
    if (counts.countSaturated && !lastCounts.countSaturated) {
        logError(
            "Impulszaehler hat UINT64_MAX erreicht; weitere Pulse "
            "koennen nicht mehr erfasst werden"
        );
    }
    lastCounts = counts;
}
