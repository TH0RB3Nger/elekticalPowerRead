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

class BufferLock {
public:
    explicit BufferLock(SemaphoreHandle_t mutex)
        : _mutex(mutex),
          _locked(_mutex != nullptr
              && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {}

    ~BufferLock() {
        if (_locked) {
            xSemaphoreGive(_mutex);
        }
    }

    bool locked() const {
        return _locked;
    }

private:
    SemaphoreHandle_t _mutex;
    bool _locked;
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
    if (_mutex == nullptr) {
        _mutex = xSemaphoreCreateMutex();
        if (_mutex == nullptr) {
            logError("Mutex fuer den Verbrauchspuffer konnte nicht erstellt werden");
            return false;
        }
    }
    BufferLock lock(_mutex);
    if (!lock.locked()) {
        logError("Verbrauchspuffer konnte nicht synchronisiert werden");
        return false;
    }

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
    BufferLock lock(_mutex);
    if (!lock.locked()) {
        logError("Verbrauchspuffer konnte nicht synchronisiert werden");
        return false;
    }
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
    BufferLock lock(_mutex);
    if (!lock.locked()) {
        logError("Verbrauchspuffer konnte nicht synchronisiert werden");
        return false;
    }
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
    BufferLock lock(_mutex);
    if (!lock.locked()) {
        logError("Sofort-Uploadstatus konnte nicht synchronisiert werden");
        return false;
    }
    return _initialized && _state.immediateUploadRequested != 0;
}

/// Prueft, ob mindestens ein Zaehler einen positiven Verbrauch gepuffert hat.
bool ConsumptionBuffer::hasPending() const {
    BufferLock lock(_mutex);
    if (!lock.locked()) {
        logError("Pufferstatus konnte nicht synchronisiert werden");
        return false;
    }
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
    BufferLock lock(_mutex);
    if (!lock.locked()) {
        logError("Pufferstatus konnte nicht synchronisiert werden");
        return 0.0;
    }
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
    uint64_t& sequence,
    bool& immediateUploadRequested
) const {
    BufferLock lock(_mutex);
    if (!lock.locked()) {
        logError("Verbrauchs-Batch konnte nicht synchronisiert werden");
        recordCount = 0;
        sequence = 0;
        immediateUploadRequested = false;
        return false;
    }
    recordCount = 0;
    sequence = 0;
    immediateUploadRequested = false;
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
    immediateUploadRequested = _state.immediateUploadRequested != 0;
    logDebug(
        "Verbrauchs-Batch zusammengestellt: Sequenz "
        + String(static_cast<unsigned long long>(sequence))
        + ", Datensaetze " + String(recordCount)
    );
    return true;
}

/// Entfernt nur bestaetigte Batchwerte und sichert die naechste Sequenz.
bool ConsumptionBuffer::confirmBatchUploaded(
    uint64_t sequence,
    const ConsumptionRecord* records,
    size_t recordCount,
    bool immediateUploadRequested
) {
    BufferLock lock(_mutex);
    if (!lock.locked()) {
        logError("Uploadbestaetigung konnte nicht synchronisiert werden");
        return false;
    }
    if (!_initialized || sequence != _state.nextSequence
        || _state.nextSequence == UINT64_MAX
        || records == nullptr || recordCount != counterCount) {
        logError("Bestaetigte Sequenz stimmt nicht mit dem Verbrauchspuffer ueberein");
        return false;
    }

    ConsumptionBufferState next = _state;
    ++next.nextSequence;
    bool seenCounter[counterCount] = {};
    for (size_t recordIndex = 0; recordIndex < recordCount; ++recordIndex) {
        size_t counterIndex = 0;
        const ConsumptionRecord& record = records[recordIndex];
        if (!findCounterIndex(record.counterId, counterIndex)
            || seenCounter[counterIndex]
            || !isfinite(record.measuredKWh)
            || record.measuredKWh < 0.0
            || record.measuredKWh > next.pendingConsumption[counterIndex]) {
            logError("Bestaetigter Batch passt nicht zum aktuellen Verbrauchspuffer");
            return false;
        }
        seenCounter[counterIndex] = true;
        next.pendingConsumption[counterIndex] -= record.measuredKWh;
    }
    bool hasUnuploadedValues = false;
    for (const double amount : next.pendingConsumption) {
        hasUnuploadedValues = hasUnuploadedValues || amount > 0.0;
    }
    next.immediateUploadRequested =
        (!immediateUploadRequested && _state.immediateUploadRequested != 0)
            || (immediateUploadRequested && hasUnuploadedValues)
        ? 1
        : 0;

    if (Config::Runtime::useNvsConsumptionBuffer) {
        if (!saveCheckpoint(next)) {
            return false;
        }
    } else {
        // Persist sequence advancement without persisting RAM-only new arrivals.
        ConsumptionBufferState checkpoint = next;
        for (double& amount : checkpoint.pendingConsumption) {
            amount = 0.0;
        }
        if (!_nvsStore.save(checkpoint)) {
            return false;
        }
        _state = next;
    }
    logDebug(
        "Bestaetigten Batch verarbeitet und Folgesequenz gesichert: "
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