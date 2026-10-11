#include "NvsStorageMonitor.h"

#include <nvs.h>

#include "Logging.h"

/**
 * @brief Misst freie NVS-Eintraege; Uploadentscheidungen trifft die Hauptschleife.
 */
/// Begrenzt die konfigurierte Schwelle auf den gueltigen Prozentbereich.
NvsStorageMonitor::NvsStorageMonitor(
    uint8_t minimumFreePercent,
    unsigned long checkIntervalMs
)
    : _minimumFreePercent(minimumFreePercent > 100 ? 100 : minimumFreePercent),
    _checkIntervalMs(checkIntervalMs) {}

/// Liest NVS-Statistik im konfigurierten Intervall und aktualisiert den Warnstatus.
bool NvsStorageMonitor::update(unsigned long now) {
    if (_lastCheck != 0 && now - _lastCheck < _checkIntervalMs) {
        return _statsAvailable;
    }
    _lastCheck = now;
    logDebugPlus("Pruefe NVS-Restkapazitaet");

    uint32_t freeEntries = 0;
    uint32_t totalEntries = 0;
    uint8_t freePercent = 0;
    nvs_stats_t stats;
    const esp_err_t result = nvs_get_stats(nullptr, &stats);
    if (result != ESP_OK || stats.total_entries == 0) {
        _statsAvailable = false;
        _critical = false;
        logError(
            "NVS-Speicherstatistik konnte nicht gelesen werden: "
            + String(result == ESP_OK ? "keine Eintraege" : esp_err_to_name(result))
        );
        return false;
    }

    totalEntries = stats.total_entries;
    freeEntries = stats.free_entries;
    const uint64_t freePercentage =
        static_cast<uint64_t>(freeEntries) * 100ULL / totalEntries;
    freePercent = static_cast<uint8_t>(freePercentage > 100 ? 100 : freePercentage);

    _statsAvailable = true;
    _critical =
        static_cast<uint64_t>(freeEntries) * 100ULL
        <= static_cast<uint64_t>(totalEntries) * _minimumFreePercent;

    logDebug(
        "NVS frei: " + String(freeEntries) + "/" + String(totalEntries)
        + " Eintraege (" + String(freePercent) + " %)"
    );
    if (_critical) {
        logWarning(
            "NVS-Freianteil bei oder unter "
            + String(_minimumFreePercent) + " %"
        );
    }
    return true;
}

/// Liefert nur bei gueltiger letzter Messung den kritischen Speicherstatus.
bool NvsStorageMonitor::isCritical() const {
    return _statsAvailable && _critical;
}
