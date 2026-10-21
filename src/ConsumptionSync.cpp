#include "ConsumptionSync.h"

#include "Logging.h"

namespace {
/// Anzahl der Messdatensaetze je atomarem Uploadintervall.
constexpr size_t expectedCounterCount = 4;
}

/// Bindet Datenbank und konfigurierten Verbrauchspuffer an die Retry-Steuerung.
ConsumptionSync::ConsumptionSync(
    DatabaseConnection& database,
    ConsumptionBuffer& buffer,
    unsigned long uploadRetryMs
)
    : _database(database),
      _buffer(buffer),
      _uploadRetryMs(uploadRetryMs) {}

/**
 * @brief Sendet den aktuellen Batch und bestaetigt ihn erst nach dem Datenbank-Commit.
 */
bool ConsumptionSync::uploadPending() {
    if (!_batchInFlight) {
        if (!_buffer.copyPendingBatch(
                _inFlightBatch,
                expectedCounterCount,
                _inFlightBatchCount,
                _inFlightSequence,
                _inFlightImmediateUploadRequested)) {
            logError("Ausstehende Verbrauchsdaten konnten nicht gelesen werden");
            return false;
        }
        _batchInFlight = true;
    }
    logInfo(
        "Speichere Messintervall der Sequenz "
        + String(static_cast<unsigned long long>(_inFlightSequence))
    );

    const String deviceId = _database.deviceId();
    if (deviceId.isEmpty()) {
        logError("ESP32-Geraete-ID konnte nicht erzeugt werden");
        return false;
    }

    if (!_database.addConsumptionBatch(
            deviceId,
            _inFlightSequence,
            _inFlightBatch,
            _inFlightBatchCount)) {
        logWarning("Datenbank-Intervall fehlgeschlagen; Verbrauchspuffer bleibt erhalten");
        return false;
    }

    if (!_buffer.confirmBatchUploaded(
            _inFlightSequence,
            _inFlightBatch,
            _inFlightBatchCount,
            _inFlightImmediateUploadRequested)) {
        logError(
            "Verbrauchspuffer konnte nach erfolgreichem Upload nicht geleert werden; "
            "derselbe Upload wird sicher wiederholt"
        );
        return false;
    }

    _batchInFlight = false;
    _inFlightBatchCount = 0;
    _inFlightSequence = 0;
    _inFlightImmediateUploadRequested = false;
    logInfo("Bestaetigtes Messintervall gespeichert; Verbrauchspuffer wurde geleert");
    return true;
}

/// Begrenzt Wiederholungsversuche nach einem fehlgeschlagenen Upload.
bool ConsumptionSync::retryAllowed(unsigned long now) const {
    const bool allowed =
        !_uploadAttempted || now - _lastUploadAttempt >= _uploadRetryMs;
    return allowed;
}

/// Speichert Zeitpunkt und Status des gestarteten Uploadversuchs.
void ConsumptionSync::markUploadAttempt(unsigned long now) {
    _uploadAttempted = true;
    _lastUploadAttempt = now;
    logDebug("Uploadversuch vorgemerkt");
}
