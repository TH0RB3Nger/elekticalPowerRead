#include "UploadScheduler.h"

#include "Logging.h"

/**
 * @brief Wandelt synchronisierte Unix-Zeit in feste lokale Uploadintervalle um.
 */
/// Prueft das Intervall und verwendet bei ungueltigen Werten den 15-Minuten-Takt.
UploadScheduler::UploadScheduler(uint8_t intervalMinutes)
    : _intervalMinutes(intervalMinutes) {
    if (_intervalMinutes == 0 || 60 % _intervalMinutes != 0) {
        logError("Uploadintervall muss ein positiver Teiler von 60 sein");
        _intervalMinutes = 15;
    }
}

/// Erkennt einen neuen faelligen Zeitschlitz und merkt den Upload bis zum Erfolg vor.
bool UploadScheduler::update(time_t currentTime) {
    if (currentTime < 0) {
        logError("Ungueltige negative Systemzeit fuer Upload-Zeitplan");
        return false;
    }

    // Unix-Zeitschlitze halten den Intervallabstand bei Sommerzeitwechseln konstant.
    const time_t intervalSeconds =
        static_cast<time_t>(_intervalMinutes) * 60;
    const time_t currentSlot = currentTime / intervalSeconds;

    if (_observedSlot == -1) {
        _observedSlot = currentSlot;
        logDebug("Upload-Zeitplan initialisiert");

        struct tm localTime;
        // Startet NTP in einer geplanten Minute, wird dieses Intervall sofort vorgemerkt.
        if (localtime_r(&currentTime, &localTime) != nullptr
            && localTime.tm_min % _intervalMinutes == 0) {
            _due = true;
            logInfo("NTP-Synchronisation erfolgte waehrend eines Messzeitpunkts");
        }
        return _due;
    }

    if (currentSlot == _observedSlot) {
        return _due;
    }

    _observedSlot = currentSlot;
    _due = true;
    struct tm localTime;
    char formattedTime[24] = "unbekannte Zeit";
    if (localtime_r(&currentTime, &localTime) != nullptr) {
        strftime(formattedTime, sizeof(formattedTime), "%H:%M:%S %Z", &localTime);
    }
    logInfo("Geplanter Messzeitpunkt erreicht: " + String(formattedTime));
    return _due;
}

/// Meldet, ob ein geplanter Upload noch bestaetigt werden muss.
bool UploadScheduler::isDue() const {
    return _due;
}

/// Loescht den vorgemerkten Zeitschlitz nach erfolgreichem Datenbank-Commit.
void UploadScheduler::markCompleted() {
    _due = false;
    logDebug("Vorgemerkter Upload-Zeitpunkt nach Erfolg abgeschlossen");
}
