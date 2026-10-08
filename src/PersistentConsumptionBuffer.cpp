#include "PersistentConsumptionBuffer.h"

#include <Preferences.h>
#include <nvs.h>

#include "Logging.h"

namespace {
/// Kennung zur Erkennung eines gueltigen Pufferschemas im Flash.
constexpr uint32_t stateMagic = 0x45504D31;

/// Feste ID des ersten Zaehlerkanals; unabhaengig von der GPIO-Zuordnung.
constexpr uint8_t firstCounterId = 4;

/// Anzahl der separat gespeicherten Zaehlerkanaele.
constexpr size_t counterCount = 4;

/// Preferences-Namensraum und Schluessel des Puffers.
constexpr char preferencesNamespace[] = "energy-cache";
constexpr char stateKey[] = "pending";

Preferences preferences;
}

bool PersistentConsumptionBuffer::begin() {
    if (!preferences.begin(preferencesNamespace, false)) {
        logError("NVS-Namensraum fuer den Verbrauchspuffer konnte nicht geoeffnet werden");
        return false;
    }

    const size_t storedSize = preferences.getBytesLength(stateKey);
    if (storedSize == 0) {
        _state.magic = stateMagic;
        _state.nextSequence = 1;
        for (double& amount : _state.pendingConsumption) {
            amount = 0.0;
        }

        _ready = true;
        if (!persist(_state)) {
            _ready = false;
            return false;
        }
        logInfo("Neuer dauerhafter Flash-Puffer angelegt");
        return true;
    }

    if (storedSize != sizeof(State)
        || preferences.getBytes(stateKey, &_state, sizeof(State)) != sizeof(State)
        || _state.magic != stateMagic
        || _state.nextSequence == 0) {
        logError(
            "Gespeicherter Flash-Puffer ist ungueltig; "
            "Daten werden nicht automatisch ueberschrieben"
        );
        return false;
    }

    for (const double amount : _state.pendingConsumption) {
        if (!isfinite(amount) || amount < 0.0) {
            logError("Flash-Puffer enthaelt einen ungueltigen Verbrauchswert");
            return false;
        }
    }

    _ready = true;
    logInfo("Dauerhafter Flash-Puffer geladen");
    return true;
}

bool PersistentConsumptionBuffer::addConsumption(
    uint8_t counterId,
    double amount
) {
    if (!_ready) {
        logError("Flash-Puffer ist nicht initialisiert");
        return false;
    }
    if (counterId < firstCounterId
        || counterId >= firstCounterId + counterCount
        || !isfinite(amount)
        || amount < 0.0) {
        logError("Ungueltiger Zaehler oder Verbrauchswert fuer Flash-Puffer");
        return false;
    }

    State next = _state;
    const size_t index = counterId - firstCounterId;
    next.pendingConsumption[index] += amount;
    if (!isfinite(next.pendingConsumption[index])) {
        logError("Verbrauchspuffer wuerde den Zahlenbereich ueberschreiten");
        return false;
    }

    return persist(next);
}

/**
 * @brief Liest freie und gesamte Eintraege der NVS-Partition.
 */
bool PersistentConsumptionBuffer::getStorageStats(
    uint32_t& freeEntries,
    uint32_t& totalEntries,
    uint8_t& freePercent
) const {
    nvs_stats_t stats;
    const esp_err_t result = nvs_get_stats(nullptr, &stats);
    if (result != ESP_OK) {
        logError(
            "NVS-Speicherstatistik konnte nicht gelesen werden: "
            + String(esp_err_to_name(result))
        );
        return false;
    }

    totalEntries = stats.total_entries;
    freeEntries = stats.free_entries;
    if (totalEntries == 0) {
        logError("NVS-Partition meldet keine nutzbaren Eintraege");
        return false;
    }

    const uint64_t percentage =
        static_cast<uint64_t>(freeEntries) * 100ULL / totalEntries;
    freePercent = static_cast<uint8_t>(percentage > 100 ? 100 : percentage);
    return true;
}

bool PersistentConsumptionBuffer::hasPending() const {
    if (!_ready) {
        return false;
    }

    for (const double amount : _state.pendingConsumption) {
        if (amount > 0.0) {
            return true;
        }
    }
    return false;
}

bool PersistentConsumptionBuffer::getPendingBatch(
    ConsumptionRecord* records,
    size_t capacity,
    size_t& recordCount,
    uint64_t& sequence
) const {
    recordCount = 0;
    sequence = 0;

    if (!_ready || records == nullptr || capacity < counterCount) {
        logError("Ungueltiger Ausgabepuffer fuer Flash-Daten");
        return false;
    }

    for (size_t index = 0; index < counterCount; ++index) {
        records[index].counterId = firstCounterId + index;
        records[index].totalConsumption = _state.pendingConsumption[index];
    }

    recordCount = counterCount;
    sequence = _state.nextSequence;
    return true;
}

bool PersistentConsumptionBuffer::markBatchUploaded(uint64_t sequence) {
    if (!_ready || sequence != _state.nextSequence
        || _state.nextSequence == UINT64_MAX) {
        logError("Bestaetigte Flash-Sequenz stimmt nicht mit dem Puffer ueberein");
        return false;
    }

    State next = _state;
    ++next.nextSequence;
    for (double& amount : next.pendingConsumption) {
        amount = 0.0;
    }

    return persist(next);
}

bool PersistentConsumptionBuffer::persist(const State& next) {
    if (!_ready && next.magic != stateMagic) {
        logError("Flash-Puffer kann nicht gespeichert werden: Zustand ungueltig");
        return false;
    }

    const size_t written = preferences.putBytes(stateKey, &next, sizeof(State));
    if (written != sizeof(State)) {
        logError("Flash-Puffer konnte nicht vollstaendig gespeichert werden");
        return false;
    }

    _state = next;
    return true;
}
