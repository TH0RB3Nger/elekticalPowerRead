#pragma once

#include <Arduino.h>

#include "PersistentConsumptionBuffer.h"

/**
 * @brief Ueberwacht NVS-Eintraege und meldet eine kritische Restkapazitaet.
 */
class NvsStorageMonitor {
public:
    /**
     * @brief Erstellt eine Ueberwachung mit Prozentgrenze und Meldeintervall.
     * @param minimumFreePercent Schwelle, bei der sofortiger Upload noetig ist.
     * @param checkIntervalMs Abstand zwischen Statistikabfragen in Millisekunden.
     */
    NvsStorageMonitor(
        uint8_t minimumFreePercent,
        unsigned long checkIntervalMs
    );

    /**
     * @brief Aktualisiert periodisch Statistik und Schwellenstatus.
     * @param buffer Flash-Puffer, dessen NVS-Partition ausgewertet wird.
     * @param now Aktueller millis()-Zeitwert.
     * @return true, wenn NVS-Statistik verfuegbar ist.
     */
    bool update(const PersistentConsumptionBuffer& buffer, unsigned long now);

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

    /// Gibt an, ob die letzte Messung die Uploadschwelle erreicht hat.
    bool _critical = false;
};
