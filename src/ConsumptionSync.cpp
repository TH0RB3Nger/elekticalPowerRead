#include "ConsumptionSync.h"

#include "Logging.h"

namespace {
/// Anzahl der Messdatensaetze je atomarem Uploadintervall.
constexpr size_t expectedCounterCount = 4;
}

ConsumptionSync::ConsumptionSync(
    DatabaseConnection& database,
    PersistentConsumptionBuffer& buffer,
    unsigned long uploadRetryMs
)
    : _database(database),
      _buffer(buffer),
      _uploadRetryMs(uploadRetryMs) {}

bool ConsumptionSync::uploadPending() {
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
    logInfo(
        "Speichere Messintervall der Sequenz "
        + String(static_cast<unsigned long long>(sequence))
    );

    const String deviceId = _database.deviceId();
    if (deviceId.isEmpty()) {
        logError("ESP32-Geraete-ID konnte nicht erzeugt werden");
        return false;
    }

    if (!_database.addConsumptionBatch(deviceId, sequence, batch, batchCount)) {
        logWarning("Datenbank-Intervall fehlgeschlagen; Flash-Puffer bleibt erhalten");
        return false;
    }

    if (!_buffer.markBatchUploaded(sequence)) {
        logError(
            "Flash-Puffer konnte nach erfolgreichem Upload nicht geleert werden; "
            "derselbe Upload wird sicher wiederholt"
        );
        return false;
    }

    logInfo("Bestaetigtes Messintervall gespeichert; Flash-Puffer wurde geleert");
    return true;
}

bool ConsumptionSync::retryAllowed(unsigned long now) const {
    const bool allowed =
        !_uploadAttempted || now - _lastUploadAttempt >= _uploadRetryMs;
    return allowed;
}

void ConsumptionSync::markUploadAttempt(unsigned long now) {
    _uploadAttempted = true;
    _lastUploadAttempt = now;
    logDebug("Uploadversuch vorgemerkt");
}
