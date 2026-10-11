#pragma once

#include <Preferences.h>

#include "ConsumptionBufferState.h"

/**
 * @brief Liest und schreibt den NVS-Zustand fuer Sequenzen und Checkpoints.
 */
class NvsConsumptionStore {
public:
    /// Oeffnet den NVS-Namensraum fuer den Verbrauchsstatus.
    bool begin();

    /**
     * @brief Laedt und validiert einen gespeicherten Zustand oder initialisiert ihn.
     * @param state Ziel fuer den geladenen oder initialisierten Zustand.
     * @param found Wird true gesetzt, wenn bereits ein NVS-Zustand existierte.
     * @return true, wenn der Zustand gueltig gelesen oder initialisiert wurde.
     */
    bool load(ConsumptionBufferState& state, bool& found);

    /// Schreibt einen vollstaendigen Zustand als atomaren NVS-Wert.
    bool save(const ConsumptionBufferState& state);

private:
    Preferences _preferences;
    bool _opened = false;
};