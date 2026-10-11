#include "ConsumptionBuffer.h"

#include <cmath>

#include "Config.h"
#include "Logging.h"

namespace {
constexpr uint8_t firstCounterId = 4;
constexpr size_t counterCount = 4;
}

bool ConsumptionBuffer::begin() {
    if (!_nvsStore.begin()) {
        return false;
    }

    bool stateFound = false;
    if (!_nvsStore.load(_state, stateFound)) {
        return false;
    }

    _initialized = true;
    if (!stateFound && Config::Runtime::useNvsConsumptionBuffer) {
        if (!saveCheckpoint(_state)) {
            _initialized = false;
            return false;
        }
        logInfo("Neuer NVS-Verbrauchspuffer angelegt");
    } else if (!stateFound) {
        logInfo("RAM-Verbrauchspuffer initialisiert; Impulse bleiben im Arbeitsspeicher");
    }
    return true;
}

bool ConsumptionBuffer::addConsumption(uint8_t counterId, double amount) {
    if (!_initialized) {
        logError("Verbrauchspuffer ist nicht initialisiert");
        return false;
    }
    if (counterId < firstCounterId
        || counterId >= firstCounterId + counterCount
        || !isfinite(amount)
        || amount < 0.0) {
        logError("Ungueltiger Zaehler oder Verbrauchswert fuer Verbrauchspuffer");
        return false;
    }

    ConsumptionBufferState next = _state;
    const size_t index = counterId - firstCounterId;
    next.pendingConsumption[index] += amount;
    if (!isfinite(next.pendingConsumption[index])
        || (amount > 0.0
            && next.pendingConsumption[index] == _state.pendingConsumption[index])) {
        logError(
            "Verbrauchspuffer wuerde den Zahlenbereich ueberschreiten "
            "oder die Addition waere nicht mehr darstellbar"
        );
        return false;
    }

    if (Config::Runtime::useNvsConsumptionBuffer) {
        return saveCheckpoint(next);
    }

    _state = next;
    return true;
}

bool ConsumptionBuffer::requestImmediateUpload() {
    if (!_initialized) {
        logError("Sofortiger Upload ohne initialisierten Verbrauchspuffer angefordert");
        return false;
    }
    if (_state.immediateUploadRequested != 0) {
        return true;
    }

    ConsumptionBufferState next = _state;
    next.immediateUploadRequested = 1;
    if (!saveCheckpoint(next)) {
        logError("Sofortiger Upload-Checkpoint konnte nicht gespeichert werden");
        return false;
    }
    logWarning("Sofortiger Upload-Checkpoint im NVS vorgemerkt");
    return true;
}

bool ConsumptionBuffer::isImmediateUploadRequested() const {
    return _initialized && _state.immediateUploadRequested != 0;
}

bool ConsumptionBuffer::hasPending() const {
    if (!_initialized) {
        return false;
    }

    for (const double amount : _state.pendingConsumption) {
        if (amount > 0.0) {
            return true;
        }
    }
    return false;
}

bool ConsumptionBuffer::copyPendingBatch(
    ConsumptionRecord* records,
    size_t capacity,
    size_t& recordCount,
    uint64_t& sequence
) const {
    recordCount = 0;
    sequence = 0;
    if (!_initialized || records == nullptr || capacity < counterCount) {
        logError("Ungueltiger Ausgabepuffer fuer Verbrauchsdaten");
        return false;
    }
    if (_state.nextSequence == UINT64_MAX) {
        logError("Datenbanksequenz ausgeschoepft; Verbrauchspuffer bleibt erhalten");
        return false;
    }

    for (size_t index = 0; index < counterCount; ++index) {
        records[index].counterId = firstCounterId + index;
        records[index].measuredKWh = _state.pendingConsumption[index];
    }

    recordCount = counterCount;
    sequence = _state.nextSequence;
    logDebug(
        "Verbrauchs-Batch zusammengestellt: Sequenz "
        + String(static_cast<unsigned long long>(sequence))
        + ", Datensaetze " + String(recordCount)
    );
    return true;
}

bool ConsumptionBuffer::confirmBatchUploaded(uint64_t sequence) {
    if (!_initialized || sequence != _state.nextSequence
        || _state.nextSequence == UINT64_MAX) {
        logError("Bestaetigte Sequenz stimmt nicht mit dem Verbrauchspuffer ueberein");
        return false;
    }

    ConsumptionBufferState next = _state;
    ++next.nextSequence;
    for (double& amount : next.pendingConsumption) {
        amount = 0.0;
    }
    next.immediateUploadRequested = 0;

    if (!saveCheckpoint(next)) {
        return false;
    }
    logDebug(
        "Bestaetigten Batch geleert und Folgesequenz gesichert: "
        + String(static_cast<unsigned long long>(sequence))
    );
    return true;
}

bool ConsumptionBuffer::saveCheckpoint(const ConsumptionBufferState& next) {
    if (!_nvsStore.save(next)) {
        return false;
    }
    _state = next;
    return true;
}