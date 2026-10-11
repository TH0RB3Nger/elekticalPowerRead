#include "RamStorageMonitor.h"

#include "Logging.h"

/**
 * @brief Meldet aktuellen und minimalen freien Heap im RAM-Puffermodus.
 */
/// Begrenzt die konfigurierte Heap-Reserve auf den gueltigen Prozentbereich.
RamStorageMonitor::RamStorageMonitor(
    uint8_t minimumFreePercent,
    unsigned long checkIntervalMs
)
    : _minimumFreePercent(minimumFreePercent > 100 ? 100 : minimumFreePercent),
      _checkIntervalMs(checkIntervalMs) {}

/// Misst den freien Heap periodisch und aktualisiert den Warnstatus.
bool RamStorageMonitor::update(unsigned long now) {
    if (_lastCheck != 0 && now - _lastCheck < _checkIntervalMs) {
        return _statsAvailable;
    }
    _lastCheck = now;

    const uint32_t freeBytes = ESP.getFreeHeap();
    const uint32_t totalBytes = ESP.getHeapSize();
    // Der Tiefstwert zeigt den kleinsten freien Heap seit dem Start.
    const uint32_t minimumFreeBytes = ESP.getMinFreeHeap();
    if (totalBytes == 0 || freeBytes > totalBytes) {
        _statsAvailable = false;
        _critical = false;
        logError("RAM-Heap-Statistik konnte nicht gelesen werden");
        return false;
    }

    const uint64_t freePercentage =
        static_cast<uint64_t>(freeBytes) * 100ULL / totalBytes;
    const uint8_t freePercent = static_cast<uint8_t>(freePercentage);
    _statsAvailable = true;
    _critical = static_cast<uint64_t>(freeBytes) * 100ULL
        <= static_cast<uint64_t>(totalBytes) * _minimumFreePercent;

    logDebug(
        "RAM frei: " + String(freeBytes) + "/" + String(totalBytes)
        + " Bytes (" + String(freePercent) + " %), Minimum seit Start: "
        + String(minimumFreeBytes) + " Bytes"
    );
    if (_critical) {
        logWarning(
            "Freier RAM bei oder unter " + String(_minimumFreePercent)
            + " %; der feste RAM-Puffer wird durch Uploads nicht verkleinert"
        );
    }
    return true;
}

/// Liefert nur bei gueltiger letzter Messung den kritischen Heap-Status.
bool RamStorageMonitor::isCritical() const {
    return _statsAvailable && _critical;
}