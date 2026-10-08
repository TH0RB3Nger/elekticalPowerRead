#pragma once

#include <Arduino.h>

#include "DatabaseConnection.h"

/**
 * @brief Speichert noch nicht bestaetigte Zaehlerwerte dauerhaft im ESP32-NVS.
 *
 * Der Speicher wird erst nach erfolgreichem Datenbank-Upload und anschliessender
 * erfolgreicher Abfrage der Datenbankstaende zurueckgesetzt.
 */
class PersistentConsumptionBuffer {
public:
    /**
     * @brief Oeffnet den NVS-Namensraum und laedt den gespeicherten Puffer.
     * @return true, wenn der Flash-Puffer bereit ist.
     */
    bool begin();

    /**
     * @brief Addiert Verbrauch zu einem Zaehler und speichert den neuen Zustand.
     * @param counterId GPIO-Nummer des Zaehlerkanals (4 bis 7).
     * @param amount Hinzuzurechnender Verbrauchswert.
     * @return true, wenn der neue Pufferstand dauerhaft gespeichert wurde.
     */
    bool addConsumption(uint8_t counterId, double amount);

    /**
     * @brief Ermittelt die freie Kapazitaet der Standard-NVS-Partition.
     * @param freeEntries Anzahl freier NVS-Eintraege.
     * @param totalEntries Gesamtzahl nutzbarer NVS-Eintraege.
     * @param freePercent Anteil der freien Eintraege in Prozent.
     * @return true, wenn die NVS-Statistik gelesen werden konnte.
     */
    bool getStorageStats(
        uint32_t& freeEntries,
        uint32_t& totalEntries,
        uint8_t& freePercent
    ) const;

    /**
     * @brief Prueft, ob noch Verbrauchswerte auf eine Uebertragung warten.
     * @return true, wenn mindestens ein Zaehler einen positiven Pufferwert hat.
     */
    bool hasPending() const;

    /**
     * @brief Kopiert alle vier ausstehenden Werte samt Upload-Sequenznummer.
     * @param records Zielpuffer fuer die vier Zaehlerwerte.
     * @param capacity Anzahl der verfuegbaren Elemente in records.
     * @param recordCount Anzahl der zurueckgegebenen Datensaetze.
     * @param sequence Sequenznummer, die fuer den idempotenten Upload benoetigt wird.
     * @return true, wenn der gesamte Puffer kopiert werden konnte.
     */
    bool getPendingBatch(
        ConsumptionRecord* records,
        size_t capacity,
        size_t& recordCount,
        uint64_t& sequence
    ) const;

    /**
     * @brief Leert einen bereits bestaetigten Puffer und erhoeht die Sequenz.
     * @param sequence Sequenz des bestaetigten Uploads.
     * @return true, wenn das geleerte Flash-Abbild gespeichert wurde.
     */
    bool markBatchUploaded(uint64_t sequence);

private:
    /// Versionierte Datenstruktur, die als einzelner NVS-Wert geschrieben wird.
    struct State {
        uint32_t magic;
        uint64_t nextSequence;
        double pendingConsumption[4];
    };

    /// NVS-Zustand im RAM; wird nur nach erfolgreichem Flash-Schreiben aktualisiert.
    State _state{};

    /// Gibt an, ob der NVS-Namensraum erfolgreich geoeffnet wurde.
    bool _ready = false;

    /**
     * @brief Speichert einen Zustand und uebernimmt ihn erst nach Erfolg im RAM.
     * @param next Der zu speichernde Zustand.
     * @return true, wenn alle Bytes im NVS gespeichert wurden.
     */
    bool persist(const State& next);
};
