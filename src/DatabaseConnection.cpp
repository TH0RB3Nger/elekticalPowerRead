#include "DatabaseConnection.h"

#include <MySQL_Generic.h>
#include <WiFi.h>
#include <cstdlib>
#include <cstdio>

#include "Logging.h"

#if __has_include("DatabaseConfig.h")
#include "DatabaseConfig.h"
#else
static char databaseHost[] = "";
static char databaseName[] = "";
static char databaseUser[] = "";
static char databasePassword[] = "";
static constexpr uint16_t databasePort = 3306;
#endif

namespace {
/// Tabellenname fuer die vier pro GPIO kumulierten Verbrauchsstaende.
constexpr char tableName[] = "energy_consumption";

/// WLAN-TCP-Client und MySQL-Protokollverbindung.
WiFiClient databaseClient;
MySQL_Connection mysqlConnection(&databaseClient);

/**
 * @brief Begrenzt Schreibzugriffe auf die vier konfigurierten GPIO-Kanaele.
 */
bool validCounterId(uint8_t counterId) {
    return counterId >= 4 && counterId <= 7;
}

/**
 * @brief Stellt WLAN- und MySQL-Verbindung sicher, ohne Erfolg zu simulieren.
 */
bool ensureConnected() {
    if (mysqlConnection.connected()) {
        return true;
    }

    if (WiFi.status() != WL_CONNECTED) {
        logWarning("Datenbankverbindung nicht moeglich: WLAN ist getrennt");
        return false;
    }

    if (databaseHost[0] == '\0' || databaseName[0] == '\0'
        || databaseUser[0] == '\0' || databasePassword[0] == '\0') {
        logError(
            "Datenbankzugangsdaten fehlen. "
            "include/DatabaseConfig.h anhand der Beispieldatei konfigurieren."
        );
        return false;
    }

    logInfo("Verbinde mit MariaDB/MySQL");
    if (!mysqlConnection.connect(
            databaseHost,
            databasePort,
            databaseUser,
            databasePassword,
            databaseName)) {
        logError("Verbindung zum MariaDB/MySQL-Server fehlgeschlagen");
        mysqlConnection.close();
        return false;
    }

    logInfo("Mit MariaDB/MySQL verbunden");
    return true;
}

/**
 * @brief Fuehrt eine einzelne nicht-transaktionale SQL-Aenderung aus.
 */
bool executeUpdate(const char* statement) {
    if (!ensureConnected()) {
        return false;
    }

    MySQL_Query query(&mysqlConnection);
    if (!query.execute(statement)) {
        logError("Datenbank-Schreibabfrage fehlgeschlagen");
        mysqlConnection.close();
        return false;
    }

    mysqlConnection.close();
    return true;
}

/**
 * @brief Fuehrt eine SQL-Anweisung innerhalb einer offenen Transaktion aus.
 */
bool executeInTransaction(const char* statement) {
    MySQL_Query query(&mysqlConnection);
    if (query.execute(statement)) {
        return true;
    }

    logError("Datenbank-Transaktion fehlgeschlagen");
    return false;
}

/**
 * @brief Bricht eine laufende Transaktion ab und schliesst den TCP-Client.
 */
void rollbackAndClose() {
    if (mysqlConnection.connected()) {
        executeInTransaction("ROLLBACK");
    }
    mysqlConnection.close();
}

/**
 * @brief Formatiert einen nichtnegativen SQL-Zahlenwert mit Dezimalpunkt.
 */
bool formatAmount(double amount, char* output, size_t outputSize) {
    if (amount < 0.0) {
        logError("Verbrauchswerte duerfen nicht negativ sein");
        return false;
    }

    const int written = snprintf(output, outputSize, "%.6f", amount);
    if (written < 0 || static_cast<size_t>(written) >= outputSize) {
        logError("Verbrauchswert kann nicht formatiert werden");
        return false;
    }
    return true;
}
}

/// Baut bei Bedarf eine Datenbankverbindung auf.
bool DatabaseConnection::connect() {
    return ensureConnected();
}

/// Meldet, ob der TCP-Client zum Datenbankserver noch verbunden ist.
bool DatabaseConnection::isConnected() const {
    return mysqlConnection.connected();
}

/**
 * @brief Erzeugt aus der ESP32-eFuse-MAC-Adresse eine stabile Geraete-ID.
 */
String DatabaseConnection::deviceId() const {
    char identifier[13];
    snprintf(
        identifier,
        sizeof(identifier),
        "%012llX",
        static_cast<unsigned long long>(ESP.getEfuseMac()) & 0xFFFFFFFFFFFFULL
    );
    return String(identifier);
}

/**
 * @brief Liest einen einzelnen kumulierten Zaehlerstand.
 */
bool DatabaseConnection::readConsumption(
    uint8_t counterId,
    double& totalConsumption
) {
    if (!validCounterId(counterId)) {
        logError("Ungueltige Zaehler-ID fuer Datenbankabfrage");
        return false;
    }
    if (!ensureConnected()) {
        return false;
    }

    char statement[128];
    snprintf(
        statement,
        sizeof(statement),
        "SELECT total_consumption FROM %s WHERE counter_id = %u",
        tableName,
        counterId
    );

    MySQL_Query query(&mysqlConnection);
    if (!query.execute(statement)) {
        logError("Datenbank-Leseabfrage fehlgeschlagen");
        mysqlConnection.close();
        return false;
    }

    if (query.get_columns() == nullptr) {
        logError("Spalteninformationen der Datenbankabfrage fehlen");
        mysqlConnection.close();
        return false;
    }

    row_values* row = query.get_next_row();
    if (row == nullptr) {
        totalConsumption = 0.0;
        mysqlConnection.close();
        return true;
    }

    totalConsumption = strtod(row->values[0], nullptr);
    mysqlConnection.close();
    return true;
}

/**
 * @brief Liest alle Zaehlerstaende in einen vom Aufrufer bereitgestellten Puffer.
 */
bool DatabaseConnection::readAll(
    ConsumptionRecord* records,
    size_t capacity,
    size_t& recordCount
) {
    recordCount = 0;
    if (records == nullptr && capacity != 0) {
        logError("Ungueltiger Ausgabepuffer fuer Datenbankabfrage");
        return false;
    }
    if (!ensureConnected()) {
        return false;
    }

    char statement[128];
    snprintf(
        statement,
        sizeof(statement),
        "SELECT counter_id, total_consumption FROM %s ORDER BY counter_id",
        tableName
    );

    MySQL_Query query(&mysqlConnection);
    if (!query.execute(statement)) {
        logError("Datenbank-Leseabfrage fehlgeschlagen");
        mysqlConnection.close();
        return false;
    }

    if (query.get_columns() == nullptr) {
        logError("Spalteninformationen der Datenbankabfrage fehlen");
        mysqlConnection.close();
        return false;
    }

    while (row_values* row = query.get_next_row()) {
        if (recordCount >= capacity) {
            logError("Ausgabepuffer zu klein fuer alle Datenbankdatensaetze");
            mysqlConnection.close();
            return false;
        }

        records[recordCount].counterId =
            static_cast<uint8_t>(strtoul(row->values[0], nullptr, 10));
        records[recordCount].totalConsumption =
            strtod(row->values[1], nullptr);
        ++recordCount;
    }

    mysqlConnection.close();
    return true;
}

/**
 * @brief Addiert einen Verbrauchswert mit einem einzelnen SQL-Upsert.
 */
bool DatabaseConnection::addConsumption(uint8_t counterId, double amount) {
    if (!validCounterId(counterId)) {
        logError("Ungueltige Zaehler-ID fuer Datenbank-Schreibvorgang");
        return false;
    }

    char formattedAmount[32];
    if (!formatAmount(amount, formattedAmount, sizeof(formattedAmount))) {
        return false;
    }

    char statement[256];
    snprintf(
        statement,
        sizeof(statement),
        "INSERT INTO %s (counter_id, total_consumption) VALUES (%u, %s) "
        "ON DUPLICATE KEY UPDATE total_consumption = "
        "total_consumption + VALUES(total_consumption)",
        tableName,
        counterId,
        formattedAmount
    );
    return executeUpdate(statement);
}

/**
 * @brief Fuegt einen absoluten Stand ein oder ersetzt den bestehenden Stand.
 */
bool DatabaseConnection::setConsumption(
    uint8_t counterId,
    double totalConsumption
) {
    if (!validCounterId(counterId)) {
        logError("Ungueltige Zaehler-ID fuer Datenbank-Schreibvorgang");
        return false;
    }

    char formattedAmount[32];
    if (!formatAmount(totalConsumption, formattedAmount, sizeof(formattedAmount))) {
        return false;
    }

    char statement[256];
    snprintf(
        statement,
        sizeof(statement),
        "INSERT INTO %s (counter_id, total_consumption) VALUES (%u, %s) "
        "ON DUPLICATE KEY UPDATE total_consumption = VALUES(total_consumption)",
        tableName,
        counterId,
        formattedAmount
    );
    return executeUpdate(statement);
}

/**
 * @brief Entfernt einen einzelnen Zaehlerdatensatz.
 */
bool DatabaseConnection::deleteCounter(uint8_t counterId) {
    if (!validCounterId(counterId)) {
        logError("Ungueltige Zaehler-ID fuer Datenbank-Loeschvorgang");
        return false;
    }

    char statement[128];
    snprintf(
        statement,
        sizeof(statement),
        "DELETE FROM %s WHERE counter_id = %u",
        tableName,
        counterId
    );
    return executeUpdate(statement);
}

bool DatabaseConnection::addConsumptionBatch(
    const String& deviceIdentifier,
    uint64_t sequence,
    const ConsumptionRecord* records,
    size_t recordCount
) {
    if (deviceIdentifier.length() != 12
        || sequence == 0
        || records == nullptr
        || recordCount != 4) {
        logError("Ungueltige Parameter fuer den Datenbank-Sammelupload");
        return false;
    }

    char quotedDeviceId[15];
    for (size_t index = 0; index < 12; ++index) {
        const char value = deviceIdentifier[index];
        if (!isxdigit(static_cast<unsigned char>(value))) {
            logError("Geraete-ID enthaelt ungueltige Zeichen");
            return false;
        }
        quotedDeviceId[index] = value;
    }
    quotedDeviceId[12] = '\0';

    if (!ensureConnected()) {
        return false;
    }

    bool transactionOpen = false;
    if (!executeInTransaction("START TRANSACTION")) {
        mysqlConnection.close();
        return false;
    }

    char statement[256];
    snprintf(
        statement,
        sizeof(statement),
        "INSERT IGNORE INTO energy_sync_state "
        "(device_id, last_sequence) VALUES ('%s', 0)",
        quotedDeviceId
    );
    if (!executeInTransaction(statement)) {
        rollbackAndClose();
        return false;
    }

    snprintf(
        statement,
        sizeof(statement),
        "SELECT last_sequence FROM energy_sync_state "
        "WHERE device_id = '%s' FOR UPDATE",
        quotedDeviceId
    );
    uint64_t lastSequence = 0;
    {
        MySQL_Query query(&mysqlConnection);
        if (!query.execute(statement) || query.get_columns() == nullptr) {
            logError("Datenbank-Sequenz konnte nicht gelesen werden");
            rollbackAndClose();
            return false;
        }

        row_values* row = query.get_next_row();
        if (row == nullptr || row->values[0] == nullptr) {
            logError("Datenbank-Synchronisationsstand fehlt");
            rollbackAndClose();
            return false;
        }
        lastSequence = strtoull(row->values[0], nullptr, 10);
    }

    if (sequence <= lastSequence) {
        if (!executeInTransaction("COMMIT")) {
            rollbackAndClose();
            return false;
        }
        mysqlConnection.close();
        logInfo("Datenbank hat dieses Upload-Buendel bereits verbucht");
        return true;
    }

    if (sequence != lastSequence + 1) {
        logError("Upload-Sequenz ist nicht fortlaufend; Buendel wird nicht addiert");
        rollbackAndClose();
        return false;
    }

    bool seenCounter[4] = {false, false, false, false};
    for (size_t index = 0; index < recordCount; ++index) {
        const uint8_t counterId = records[index].counterId;
        if (!validCounterId(counterId) || seenCounter[counterId - 4]) {
            logError("Upload-Buendel enthaelt ungueltige oder doppelte Zaehler-IDs");
            rollbackAndClose();
            return false;
        }
        seenCounter[counterId - 4] = true;

        char formattedAmount[32];
        if (!formatAmount(records[index].totalConsumption, formattedAmount, sizeof(formattedAmount))) {
            rollbackAndClose();
            return false;
        }

        snprintf(
            statement,
            sizeof(statement),
            "INSERT INTO %s (counter_id, total_consumption) VALUES (%u, %s) "
            "ON DUPLICATE KEY UPDATE total_consumption = "
            "total_consumption + VALUES(total_consumption)",
            tableName,
            counterId,
            formattedAmount
        );
        if (!executeInTransaction(statement)) {
            rollbackAndClose();
            return false;
        }
    }

    snprintf(
        statement,
        sizeof(statement),
        "UPDATE energy_sync_state SET last_sequence = %llu "
        "WHERE device_id = '%s'",
        static_cast<unsigned long long>(sequence),
        quotedDeviceId
    );
    if (!executeInTransaction(statement)
        || !executeInTransaction("COMMIT")) {
        rollbackAndClose();
        return false;
    }

    mysqlConnection.close();
    logInfo("Datenbank-Sammelupload atomar verbucht");
    return true;
}
