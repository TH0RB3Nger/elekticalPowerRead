#pragma once

#include <Arduino.h>

/**
 * @brief Ueberwacht den freien ESP32-Heap fuer den RAM-Puffermodus.
 */
class RamStorageMonitor {
public:
    /**
     * @brief Erstellt eine Heap-Ueberwachung mit Reservegrenze und Intervall.
     * @param minimumFreePercent Mindestanteil freien Heaps in Prozent.
     * @param checkIntervalMs Abstand zwischen Messungen in Millisekunden.
     */
    RamStorageMonitor(
        uint8_t minimumFreePercent,
        unsigned long checkIntervalMs
    );

    /// Aktualisiert periodisch freie, gesamte und minimale Heap-Menge.
    bool update(unsigned long now);

    /// Meldet, ob die letzte Messung die konfigurierte Reservegrenze erreichte.
    bool isCritical() const;

private:
    uint8_t _minimumFreePercent;
    unsigned long _checkIntervalMs;
    unsigned long _lastCheck = 0;
    bool _statsAvailable = false;
    bool _critical = false;
};