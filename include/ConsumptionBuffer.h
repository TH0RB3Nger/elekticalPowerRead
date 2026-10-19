#pragma once

#include <Arduino.h>

#include "ConsumptionBufferState.h"
#include "DatabaseConnection.h"
#include "NvsConsumptionStore.h"

/**
 * @brief Haelt ausstehende Verbrauchswerte im konfigurierten RAM- oder NVS-Modus.
 *
 * Der RAM-Modus puffert Messwerte fluechtig. Sequenzen und Ueberlauf-Checkpoints
 * werden unabhaengig davon im NVS gesichert.
 */
class ConsumptionBuffer {
public:
    /// Laedt vorhandenen NVS-Zustand oder initialisiert einen neuen Puffer.
    bool begin();

    /**
     * @brief Addiert den Verbrauch eines Zaehlerkanals zum aktiven Puffer.
        * @param counterId Konfigurierte Datenbank-ID des Zaehlerkanals.
     * @param amount Hinzuzurechnender Verbrauchswert in kWh.
     * @return true, wenn der neue Pufferstand angenommen wurde.
     */
    bool addConsumption(uint8_t counterId, double amount);

    /// Sichert einen sofortigen Upload-Checkpoint und markiert ihn als ausstehend.
    bool requestImmediateUpload();

    /// Meldet, ob ein sofortiger Upload vorgemerkt ist.
    bool isImmediateUploadRequested() const;

    /// Meldet, ob mindestens ein positiver Verbrauchswert aussteht.
    bool hasPending() const;

    /// Liefert die Summe der noch nicht bestaetigten Verbrauchswerte in kWh.
    double getPendingConsumptionKWh() const;

    /**
     * @brief Kopiert die vier Kanalwerte mit der naechsten Datenbanksequenz.
     * @param records Zielpuffer fuer die Verbrauchssaetze.
     * @param capacity Anzahl der verfuegbaren Elemente in records.
     * @param recordCount Anzahl der zurueckgegebenen Saetze.
     * @param sequence Sequenznummer fuer den idempotenten Datenbankupload.
     * @return true, wenn ein vollstaendiger Batch kopiert wurde.
     */
    bool copyPendingBatch(
        ConsumptionRecord* records,
        size_t capacity,
        size_t& recordCount,
        uint64_t& sequence
    ) const;

    /**
     * @brief Bestaetigt einen Commit, leert Werte und sichert die Folgesequenz.
     * @param sequence Sequenznummer des bestaetigten Batches.
     * @return true, wenn Zustand und Folgesequenz uebernommen wurden.
     */
    bool confirmBatchUploaded(uint64_t sequence);

private:
    /// NVS wird fuer Sequenznummern und seltene Checkpoints in beiden Modi genutzt.
    NvsConsumptionStore _nvsStore;

    /// Aktueller RAM-Arbeitszustand; im NVS-Modus wird jede Aenderung gespiegelt.
    ConsumptionBufferState _state{};

    /// Gibt an, ob der Puffer erfolgreich initialisiert wurde.
    bool _initialized = false;

    /// Speichert einen NVS-Checkpoint und aktualisiert den RAM-Zustand erst danach.
    bool saveCheckpoint(const ConsumptionBufferState& next);
};