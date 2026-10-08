#pragma once

#include <Arduino.h>

/**
 * @brief Verbrauch eines Zaehlers seit dem vorigen erfolgreichen Messpunkt.
 */
struct ConsumptionRecord {
    /// Feste Datenbank-ID des Zaehlerkanals (4 bis 7).
    uint8_t counterId;
    /// Gemessener Verbrauch in kWh seit dem vorigen Zeitstempel.
    double measuredKWh;
};

/**
 * @brief Verbindet die Firmware mit der Zeitreihentabelle.
 */
class DatabaseConnection {
public:
    /// Stellt bei verfuegbarem WLAN eine Verbindung zum Datenbankserver her.
    bool connect();

    /// Prueft, ob die zugrunde liegende Datenbankverbindung aktiv ist.
    bool isConnected() const;

    /**
     * @brief Liefert eine stabile Kennung aus der eFuse-MAC-Adresse.
     * @return Zwölfstellige hexadezimale ESP32-Kennung.
     */
    String deviceId() const;

    /**
     * @brief Speichert ein Messintervall genau einmal und atomar.
     * @param deviceId Stabile Kennung des sendenden ESP32.
     * @param sequence Persistente Sequenznummer des Buendels.
     * @param records Verbrauch seit dem jeweils vorigen Zeitstempel.
     * @param recordCount Anzahl der Eintraege in records.
     * @return true, wenn das Intervall neu oder bereits zuvor gespeichert wurde.
     */
    bool addConsumptionBatch(
        const String& deviceId,
        uint64_t sequence,
        const ConsumptionRecord* records,
        size_t recordCount
    );
};
