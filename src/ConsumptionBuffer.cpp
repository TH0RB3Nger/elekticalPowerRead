#include "ConsumptionBuffer.h"

#include <cmath>

#include "Config.h"
#include "Logging.h"

/**
 * @brief Aggregiert Messwerte im RAM und schreibt je nach Modus NVS-Checkpoints.
 */
namespace {
constexpr size_t counterCount = 4;
constexpr uint8_t counterIds[counterCount] = {
    Config::Counters::databaseId1,
    Config::Counters::databaseId2,
    Config::Counters::databaseId3,
    Config::Counters::databaseId4
};

bool findCounterIndex(uint8_t counterId, size_t& index) {
    for (size_t candidate = 0; candidate < counterCount; ++candidate) {
        if (counterIds[candidate] == counterId) {
            index = candidate;
            return true;
        }
    }
    return false;
}
}

/// Initialisiert den NVS-Status und laedt ausstehende Werte, falls vorhanden.
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

/// Addiert einen Messwert und speichert ihn nur im konfigurierten NVS-Modus sofort.
bool ConsumptionBuffer::addConsumption(uint8_t counterId, double amount) {
    if (!_initialized) {
        logError("Verbrauchspuffer ist nicht initialisiert");
        return false;
    }
    size_t index = 0;
    if (!findCounterIndex(counterId, index)
        || !isfinite(amount) || amount < 0.0) {
        logError("Ungueltiger Zaehler oder Verbrauchswert fuer Verbrauchspuffer");
        return false;
    }

    ConsumptionBufferState next = _state;
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

    // Im RAM-Modus wird der Messwert ohne Flash-Schreibvorgang uebernommen.
    if (Config::Runtime::useNvsConsumptionBuffer) {
        return saveCheckpoint(next);
    }

    _state = next;
    return true;
}

/// Speichert eine sofortige Uploadanforderung als NVS-Checkpoint.
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

/// Prueft, ob ein persistenter Sofort-Upload-Checkpoint aussteht.
bool ConsumptionBuffer::isImmediateUploadRequested() const {
    return _initialized && _state.immediateUploadRequested != 0;
}

/// Prueft, ob mindestens ein Zaehler einen positiven Verbrauch gepuffert hat.
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

/// Addiert die vier noch nicht bestaetigten Kanalwerte fuer die Statusanzeige.
double ConsumptionBuffer::getPendingConsumptionKWh() const {
    if (!_initialized) {
        return 0.0;
    }

    double total = 0.0;
    for (const double amount : _state.pendingConsumption) {
        total += amount;
    }
    return total;
}

/// Kopiert alle Kanalwerte zusammen mit der aktuellen Uploadsequenz.
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
        records[index].counterId = counterIds[index];
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

/// Leert einen bestaetigten Batch und sichert die naechste Sequenz.
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

    // Die Folgesequenz bleibt in beiden Modi fuer Neustart-Wiederholungen erhalten.
    if (!saveCheckpoint(next)) {
        return false;
    }
    logDebug(
        "Bestaetigten Batch geleert und Folgesequenz gesichert: "
        + String(static_cast<unsigned long long>(sequence))
    );
    return true;
}

/// Uebernimmt den neuen RAM-Zustand erst nach erfolgreichem NVS-Schreibvorgang.
bool ConsumptionBuffer::saveCheckpoint(const ConsumptionBufferState& next) {
    if (!_nvsStore.save(next)) {
        return false;
    }
    _state = next;
    return true;
}