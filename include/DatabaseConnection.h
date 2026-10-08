#pragma once

#include <Arduino.h>

/**
 * @brief Ein kumulierter Verbrauchsstand fuer einen einzelnen GPIO-Zaehler.
 */
struct ConsumptionRecord {
    /// GPIO-Nummer des Zaehlerkanals (4 bis 7).
    uint8_t counterId;
    /// Kumulierte Verbrauchsmenge fuer diesen Kanal.
    double totalConsumption;
};

/**
 * @brief Stellt Lese-, Schreib- und Synchronisationsfunktionen bereit.
 */
class DatabaseConnection {
public:
    /// Stellt bei verfuegbarem WLAN eine Verbindung zum Datenbankserver her.
    bool connect();

    /// Prueft, ob die zugrunde liegende Datenbankverbindung aktiv ist.
    bool isConnected() const;

    /// Liest den kumulierten Stand eines einzelnen Zaehlerkanals.
    bool readConsumption(uint8_t counterId, double& totalConsumption);

    /// Liest die kumulierten Staende aller vorhandenen Zaehlerkanaele.
    bool readAll(
        ConsumptionRecord* records,
        size_t capacity,
        size_t& recordCount
    );

    /// Addiert einen Verbrauchswert zum Datenbankstand eines Kanals.
    bool addConsumption(uint8_t counterId, double amount);

    /// Setzt den Datenbankstand eines Kanals auf einen absoluten Wert.
    bool setConsumption(uint8_t counterId, double totalConsumption);

    /// Loescht den Datensatz eines Zaehlerkanals.
    bool deleteCounter(uint8_t counterId);

    /**
     * @brief Liefert eine stabile Kennung aus der eFuse-MAC-Adresse.
     * @return Zwölfstellige hexadezimale ESP32-Kennung.
     */
    String deviceId() const;

    /**
     * @brief Addiert ein ganzes Upload-Buendel genau einmal und atomar.
     * @param deviceId Stabile Kennung des sendenden ESP32.
     * @param sequence Persistente Sequenznummer des Buendels.
     * @param records Verbrauchsdifferenzen je Zaehlerkanal.
     * @param recordCount Anzahl der Eintraege in records.
     * @return true, wenn das Buendel neu oder bereits zuvor verbucht wurde.
     */
    bool addConsumptionBatch(
        const String& deviceId,
        uint64_t sequence,
        const ConsumptionRecord* records,
        size_t recordCount
    );
};
