#pragma once

#include <Arduino.h>

/**
 * @brief Ueberwacht die freie Kapazitaet der NVS-Partition.
 */
class NvsStorageMonitor {
public:
    /**
    * @brief Erstellt eine Ueberwachung mit NVS-Schwelle und Pruefintervall.
    * @param minimumFreePercent Schwelle fuer eine kritische NVS-Restkapazitaet.
     * @param checkIntervalMs Abstand zwischen Statistikabfragen in Millisekunden.
     */
    NvsStorageMonitor(
        uint8_t minimumFreePercent,
        unsigned long checkIntervalMs
    );

    /**
     * @brief Aktualisiert periodisch NVS-Statistik und Schwellenstatus.
     * @param now Aktueller millis()-Zeitwert.
     * @return true, wenn NVS-Statistik verfuegbar ist.
     */
    bool update(unsigned long now);

    /// Meldet, ob die letzte Statistik die konfigurierte Schwelle erreicht hat.
    bool isCritical() const;

private:
    /// Kritischer Anteil an freien NVS-Eintraegen in Prozent.
    uint8_t _minimumFreePercent;

    /// Abstand zwischen NVS-Statistikabfragen in Millisekunden.
    unsigned long _checkIntervalMs;

    /// Zeitpunkt der letzten erfolgreichen oder fehlgeschlagenen Pruefung.
    unsigned long _lastCheck = 0;

    /// Ergebnis der letzten NVS-Statistikabfrage.
    bool _statsAvailable = false;

    /// Gibt an, ob die letzte Messung die NVS-Warnschwelle erreicht hat.
    bool _critical = false;
};
