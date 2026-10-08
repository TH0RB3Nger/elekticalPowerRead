#pragma once

#include <Arduino.h>

#include "DatabaseConnection.h"
#include "PersistentConsumptionBuffer.h"

/**
 * @brief Koordiniert Messintervall-Upload und commitabhaengige Pufferloeschung.
 */
class ConsumptionSync {
public:
    /**
     * @brief Verknuepft Datenbank und dauerhaften Verbrauchspuffer.
     * @param database Datenbankzugriff fuer Lesen und Schreiben.
     * @param buffer Persistenter Flash-Puffer der noch nicht bestaetigten Werte.
     * @param uploadRetryMs Abstand zwischen Upload-Wiederholungen.
     */
    ConsumptionSync(
        DatabaseConnection& database,
        PersistentConsumptionBuffer& buffer,
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
    /// Datenbankzugriff und Flash-Puffer bleiben im Hauptprogramm im Besitz.
    DatabaseConnection& _database;
    PersistentConsumptionBuffer& _buffer;

    /// Konfigurierbarer Zeitabstand fuer Upload-Wiederholungen.
    unsigned long _uploadRetryMs;

    /// Zustand und Zeitpunkt des letzten Datenbank-Uploadversuchs.
    bool _uploadAttempted = false;
    unsigned long _lastUploadAttempt = 0;
};
