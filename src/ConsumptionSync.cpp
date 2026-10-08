#include "ConsumptionSync.h"

#include "Logging.h"

namespace {
/// Anzahl der erwarteten Datenbankdatensaetze fuer die vier Zaehlerkanaele.
constexpr size_t expectedCounterCount = 4;
}

ConsumptionSync::ConsumptionSync(
    DatabaseConnection& database,
    PersistentConsumptionBuffer& buffer,
    unsigned long databaseReadRetryMs,
    unsigned long uploadRetryMs
)
    : _database(database),
      _buffer(buffer),
      _databaseReadRetryMs(databaseReadRetryMs),
      _uploadRetryMs(uploadRetryMs) {}

bool ConsumptionSync::initialize(unsigned long now) {
    if (_initialized) {
        return true;
    }
    if (_databaseReadAttempted
        && now - _lastDatabaseReadAttempt < _databaseReadRetryMs) {
        return _initialized;
    }

    _databaseReadAttempted = true;
    _lastDatabaseReadAttempt = now;
    ConsumptionRecord records[expectedCounterCount];
    size_t recordCount = 0;
    if (!_database.readAll(records, expectedCounterCount, recordCount)) {
        logWarning("Datenbankstaende konnten noch nicht gelesen werden");
        return false;
    }

    if (recordCount != expectedCounterCount) {
        logError(
            "Die Datenbank muss genau vier Zaehlerdatensaetze "
            "(IDs 4 bis 7) enthalten"
        );
        return false;
    }

    for (size_t index = 0; index < recordCount; ++index) {
        if (records[index].counterId != index + 4) {
            logError(
                "Die Datenbank enthaelt nicht die erwarteten Zaehler-IDs "
                "(IDs 4 bis 7)"
            );
            return false;
        }
        logInfo(
            "Datenbankstand Zaehler " + String(records[index].counterId)
            + ": " + String(records[index].totalConsumption, 3)
        );
    }
    _initialized = true;
    return true;
}

bool ConsumptionSync::isInitialized() const {
    return _initialized;
}

bool ConsumptionSync::uploadPending() {
    if (!_initialized || !_buffer.hasPending()) {
        return false;
    }

    ConsumptionRecord batch[expectedCounterCount];
    size_t batchCount = 0;
    uint64_t sequence = 0;
    if (!_buffer.getPendingBatch(
            batch,
            expectedCounterCount,
            batchCount,
            sequence)) {
        logError("Ausstehende Flash-Daten konnten nicht gelesen werden");
        return false;
    }

    const String deviceId = _database.deviceId();
    if (deviceId.isEmpty()) {
        logError("ESP32-Geraete-ID konnte nicht erzeugt werden");
        return false;
    }

    if (!_database.addConsumptionBatch(deviceId, sequence, batch, batchCount)) {
        logWarning("Datenbank-Upload fehlgeschlagen; Flash-Puffer bleibt erhalten");
        return false;
    }

    ConsumptionRecord confirmedRecords[expectedCounterCount];
    size_t confirmedCount = 0;
    if (!_database.readAll(
            confirmedRecords,
            expectedCounterCount,
            confirmedCount)
        || confirmedCount != expectedCounterCount) {
        logWarning(
            "Upload wurde verarbeitet, aber die Datenbankbestaetigung "
            "ist unvollstaendig; Flash-Puffer bleibt zur sicheren Wiederholung erhalten"
        );
        return false;
    }

    for (size_t index = 0; index < confirmedCount; ++index) {
        logInfo(
            "Bestaetigter Datenbankstand Zaehler "
            + String(confirmedRecords[index].counterId) + ": "
            + String(confirmedRecords[index].totalConsumption, 3)
        );
    }

    if (!_buffer.markBatchUploaded(sequence)) {
        logError(
            "Flash-Puffer konnte nach erfolgreichem Upload nicht geleert werden; "
            "derselbe Upload wird sicher wiederholt"
        );
        return false;
    }

    logInfo("Bestaetigter Flash-Puffer wurde geleert");
    return true;
}

bool ConsumptionSync::retryAllowed(unsigned long now) const {
    return !_uploadAttempted || now - _lastUploadAttempt >= _uploadRetryMs;
}

void ConsumptionSync::markUploadAttempt(unsigned long now) {
    _uploadAttempted = true;
    _lastUploadAttempt = now;
}
