#pragma once

#include <Arduino.h>

#include "DatabaseConnection.h"
#include "PersistentConsumptionBuffer.h"

/**
 * @brief Koordiniert Datenbankstartwerte, Upload, Bestaetigung und Pufferruecksetzung.
 */
class ConsumptionSync {
public:
    /**
     * @brief Verknuepft Datenbank und dauerhaften Verbrauchspuffer.
     * @param database Datenbankzugriff fuer Lesen und Schreiben.
     * @param buffer Persistenter Flash-Puffer der noch nicht bestaetigten Werte.
     * @param databaseReadRetryMs Abstand zwischen Startwert-Leseversuchen.
     * @param uploadRetryMs Abstand zwischen Upload-Wiederholungen.
     */
    ConsumptionSync(
        DatabaseConnection& database,
        PersistentConsumptionBuffer& buffer,
        unsigned long databaseReadRetryMs,
        unsigned long uploadRetryMs
    );

    /**
     * @brief Liest die vier Datenbankstaende, bevor Uploads erlaubt werden.
     * @param now Aktueller millis()-Zeitwert.
     * @return true, wenn alle vier Ausgangsstaende gelesen wurden.
     */
    bool initialize(unsigned long now);

    /// Meldet, ob die Startwerte aller vier Zaehler verfuegbar sind.
    bool isInitialized() const;

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

    /// Konfigurierbare Zeitabstaende fuer Wiederholungen.
    unsigned long _databaseReadRetryMs;
    unsigned long _uploadRetryMs;

    /// Zustand und Zeitpunkte der Datenbank-Synchronisation.
    bool _initialized = false;
    bool _databaseReadAttempted = false;
    bool _uploadAttempted = false;
    unsigned long _lastDatabaseReadAttempt = 0;
    unsigned long _lastUploadAttempt = 0;
};
