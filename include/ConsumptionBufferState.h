#pragma once

#include <Arduino.h>

/**
 * @brief Gemeinsamer Pufferzustand fuer RAM-Verarbeitung und NVS-Checkpoints.
 */
struct ConsumptionBufferState {
    /// Kennung des binaeren Zustandsformats fuer NVS-Migrationen.
    uint32_t magic;
    /// Sequenznummer des naechsten Datenbank-Batches.
    uint64_t nextSequence;
    /// Noch nicht bestaetigter Verbrauch je Zaehlerkanal 1 bis 4 in kWh.
    double pendingConsumption[4];
    /// Markiert einen persistent vorgemerkten sofortigen Upload.
    uint8_t immediateUploadRequested;
};