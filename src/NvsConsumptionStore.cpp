#include "NvsConsumptionStore.h"

#include <cmath>

#include "Logging.h"

/**
 * @brief Speichert den versionierten Verbrauchszustand und migriert alte NVS-Datensaetze.
 */
namespace {
/// Kennungen unterscheiden das aktuelle und das vorherige binaere Datenformat.
constexpr uint32_t currentStateMagic = 0x45504D32;
constexpr uint32_t legacyStateMagic = 0x45504D31;
constexpr size_t counterCount = 4;
constexpr char preferencesNamespace[] = "energy-cache";
constexpr char stateKey[] = "pending";

struct LegacyConsumptionBufferState {
    /// Feldreihenfolge und Typen muessen dem vorherigen NVS-Datensatz entsprechen.
    uint32_t magic;
    uint64_t nextSequence;
    double pendingConsumption[counterCount];
};

/// Prueft alle gespeicherten Verbrauchswerte auf gueltige nichtnegative Zahlen.
bool validAmounts(const double* amounts) {
    for (size_t index = 0; index < counterCount; ++index) {
        if (!isfinite(amounts[index]) || amounts[index] < 0.0) {
            return false;
        }
    }
    return true;
}
}

/// Oeffnet den Preferences-Namensraum fuer Sequenz- und Pufferdaten.
bool NvsConsumptionStore::begin() {
    logDebug("Oeffne NVS-Namensraum fuer Sequenzstatus und Checkpoints");
    _opened = _preferences.begin(preferencesNamespace, false);
    if (!_opened) {
        logError("NVS-Namensraum fuer Sequenzstatus konnte nicht geoeffnet werden");
    }
    return _opened;
}

/// Laedt den aktuellen Datensatz oder migriert das vorherige NVS-Format.
bool NvsConsumptionStore::load(
    ConsumptionBufferState& state,
    bool& found
) {
    found = false;
    if (!_opened) {
        logError("NVS-Zustand kann vor dem Oeffnen des Namensraums nicht geladen werden");
        return false;
    }

    const size_t storedSize = _preferences.getBytesLength(stateKey);
    if (storedSize == 0) {
        state = {};
        state.magic = currentStateMagic;
        state.nextSequence = 1;
        logDebug("Kein NVS-Zustand gefunden; initialisiere Sequenzstatus");
        return true;
    }

    // Migriert den alten Datensatz und behaelt ausstehende Messwerte bei.
    if (storedSize == sizeof(LegacyConsumptionBufferState)) {
        LegacyConsumptionBufferState legacy{};
        if (_preferences.getBytes(stateKey, &legacy, sizeof(legacy)) != sizeof(legacy)
            || legacy.magic != legacyStateMagic
            || legacy.nextSequence == 0
            || !validAmounts(legacy.pendingConsumption)) {
            logError("Vorheriger NVS-Zustand ist ungueltig; Werte bleiben erhalten");
            return false;
        }

        state = {};
        state.magic = currentStateMagic;
        state.nextSequence = legacy.nextSequence;
        for (size_t index = 0; index < counterCount; ++index) {
            state.pendingConsumption[index] = legacy.pendingConsumption[index];
        }
        if (!save(state)) {
            return false;
        }
        found = true;
        logInfo("NVS-Zustand verlustfrei auf das aktuelle Format migriert");
        return true;
    }

    if (storedSize != sizeof(ConsumptionBufferState)
        || _preferences.getBytes(stateKey, &state, sizeof(state)) != sizeof(state)
        || state.magic != currentStateMagic
        || state.nextSequence == 0
        || state.immediateUploadRequested > 1
        || !validAmounts(state.pendingConsumption)) {
        logError("Gespeicherter NVS-Zustand ist ungueltig; er bleibt unveraendert");
        return false;
    }

    found = true;
    logInfo(
        "NVS-Zustand geladen; naechste Datenbanksequenz "
        + String(static_cast<unsigned long long>(state.nextSequence))
    );
    logDebugPlus(
        "Geladene Verbrauchswerte: ID 4=" + String(state.pendingConsumption[0], 6)
        + ", ID 5=" + String(state.pendingConsumption[1], 6)
        + ", ID 6=" + String(state.pendingConsumption[2], 6)
        + ", ID 7=" + String(state.pendingConsumption[3], 6)
    );
    return true;
}

/// Validiert und schreibt den kompletten Zustand als einzelnen NVS-Wert.
bool NvsConsumptionStore::save(const ConsumptionBufferState& state) {
    if (!_opened
        || state.magic != currentStateMagic
        || state.nextSequence == 0
        || state.immediateUploadRequested > 1
        || !validAmounts(state.pendingConsumption)) {
        logError("Ungueltiger Zustand kann nicht als NVS-Checkpoint gespeichert werden");
        return false;
    }

    logDebugPlus(
        "Schreibe " + String(sizeof(state)) + " Bytes als NVS-Checkpoint"
    );
    if (_preferences.putBytes(stateKey, &state, sizeof(state)) != sizeof(state)) {
        logError("NVS-Checkpoint konnte nicht vollstaendig gespeichert werden");
        return false;
    }
    logDebugPlus("NVS-Checkpoint erfolgreich aktualisiert");
    return true;
}