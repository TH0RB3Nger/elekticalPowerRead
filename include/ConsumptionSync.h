#pragma once

#include <Arduino.h>

#include "ConsumptionBuffer.h"
#include "DatabaseConnection.h"

/**
 * @brief Koordiniert Messintervall-Upload und commitabhaengige Pufferloeschung.
 */
class ConsumptionSync {
public:
    /**
        * @brief Verknuepft Datenbank und konfigurierten Verbrauchspuffer.
     * @param database Datenbankzugriff fuer Lesen und Schreiben.
        * @param buffer RAM-/NVS-Puffer der noch nicht bestaetigten Werte.
     * @param uploadRetryMs Abstand zwischen Upload-Wiederholungen.
     */
    ConsumptionSync(
        DatabaseConnection& database,
        ConsumptionBuffer& buffer,
        unsigned long uploadRetryMs
    );

    /**
     * @brief Sendet den aktuellen Puffer, bestaetigt die Daten und loescht ihn.
     * @return true, wenn der Batch erfolgreich bestaetigt und geloescht wurde.
     */
    bool uploadPending();

    /**
     * @brief Prueft, ob ein weiterer fehlgeschlagener Upload versucht werden darf.
     * @param now Aktueller millis()-Zeitwert.
     */
    bool retryAllowed(unsigned long now) const;

    /**
     * @brief Merkt einen Uploadversuch fuer die Retry-Begrenzung vor.
     * @param now Aktueller millis()-Zeitwert.
     */
    void markUploadAttempt(unsigned long now);

private:
    /// Datenbankzugriff und Verbrauchspuffer bleiben im Hauptprogramm im Besitz.
    DatabaseConnection& _database;
    ConsumptionBuffer& _buffer;

    /// Konfigurierbarer Zeitabstand fuer Upload-Wiederholungen.
    unsigned long _uploadRetryMs;

    /// Zustand und Zeitpunkt des letzten Datenbank-Uploadversuchs.
    bool _uploadAttempted = false;
    unsigned long _lastUploadAttempt = 0;
};
