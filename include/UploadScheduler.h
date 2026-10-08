#pragma once

#include <Arduino.h>
#include <time.h>

/**
 * @brief Ermittelt lokale feste Upload-Zeitpunkte auf einem Minutentakt.
 */
class UploadScheduler {
public:
    /**
     * @brief Erstellt einen Zeitplan mit dem gewuenschten Abstand in Minuten.
     * @param intervalMinutes Uploadabstand; muss eine positive Stunde teilen.
     */
    explicit UploadScheduler(uint8_t intervalMinutes);

    /**
     * @brief Aktualisiert den Zeitplan und merkt faellige Messpunkte vor.
     * @param currentTime Synchronisierte Unix-Zeit.
     * @return true, wenn ein zeitgesteuerter Messpunkt ansteht.
     */
    bool update(time_t currentTime);

    /**
     * @brief Meldet, ob ein faelliger Zeitplan-Upload vorgemerkt ist.
     */
    bool isDue() const;

    /**
     * @brief Loescht den vorgemerkten Zeitpunkt nach bestaetigtem Upload.
     */
    void markCompleted();

private:
    /// Uploadabstand in Minuten zwischen den lokalen Stundenmarken.
    uint8_t _intervalMinutes;

    /// Epoch-Slot des letzten verarbeiteten Zeitpunkts.
    time_t _observedSlot = -1;

    /// Bleibt nach Erreichen eines Zeitpunkts bis zum Erfolg gesetzt.
    bool _due = false;
};
