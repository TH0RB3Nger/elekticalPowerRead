#include "NvsStorageMonitor.h"

#include "Logging.h"

NvsStorageMonitor::NvsStorageMonitor(
    uint8_t minimumFreePercent,
    unsigned long checkIntervalMs
)
    : _minimumFreePercent(minimumFreePercent > 100 ? 100 : minimumFreePercent),
      _checkIntervalMs(checkIntervalMs) {}

bool NvsStorageMonitor::update(
    const PersistentConsumptionBuffer& buffer,
    unsigned long now
) {
    if (_lastCheck != 0 && now - _lastCheck < _checkIntervalMs) {
        return _statsAvailable;
    }
    _lastCheck = now;

    uint32_t freeEntries = 0;
    uint32_t totalEntries = 0;
    uint8_t freePercent = 0;
    if (!buffer.getStorageStats(freeEntries, totalEntries, freePercent)) {
        _statsAvailable = false;
        _critical = false;
        return false;
    }

    _statsAvailable = true;
    _critical =
        static_cast<uint64_t>(freeEntries) * 100ULL
        <= static_cast<uint64_t>(totalEntries) * _minimumFreePercent;

    logInfo(
        "NVS frei: " + String(freeEntries) + "/" + String(totalEntries)
        + " Eintraege (" + String(freePercent) + " %)"
    );
    if (_critical) {
        logWarning(
            "NVS-Freianteil bei oder unter "
            + String(_minimumFreePercent)
            + " %; zusaetzlicher Upload wird ausgeloest"
        );
    }
    return true;
}

bool NvsStorageMonitor::isCritical() const {
    return _statsAvailable && _critical;
}
