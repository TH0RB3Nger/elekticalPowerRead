#include "DatabaseConnection.h"

#include <MySQL_Generic.h>
#include <WiFi.h>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "Logging.h"

#if __has_include("Config.h")
#include "Config.h"
#else
#error "Config.h fehlt. Kopiere include/Config.h.example nach include/Config.h und trage deine Werte ein."
#endif

namespace {
constexpr size_t expectedCounterCount = 4;

WiFiClient databaseClient;
MySQL_Connection mysqlConnection(&databaseClient);

bool validCounterId(uint8_t counterId) {
    return counterId >= 4 && counterId <= 7;
}

bool parseUint64(const char* value, uint64_t& parsed) {
    if (value == nullptr || *value == '\0') {
        return false;
    }

    uint64_t result = 0;
    for (const char* digit = value; *digit != '\0'; ++digit) {
        if (*digit < '0' || *digit > '9') {
            return false;
        }
        const uint8_t numericDigit = static_cast<uint8_t>(*digit - '0');
        if (result > (UINT64_MAX - numericDigit) / 10ULL) {
            return false;
        }
        result = result * 10ULL + numericDigit;
    }

    parsed = result;
    return true;
}

bool ensureConnected() {
    if (mysqlConnection.connected()) {
        logDebugPlus("Datenbankverbindung ist bereits aktiv");
        return true;
    }
    if (WiFi.status() != WL_CONNECTED) {
        logWarning("Datenbankverbindung nicht moeglich: WLAN ist getrennt");
        return false;
    }
    if (Config::Database::host[0] == '\0'
        || Config::Database::name[0] == '\0'
        || Config::Database::user[0] == '\0'
        || Config::Database::password[0] == '\0'
        || strncmp(Config::Database::host, "REPLACE_", 8) == 0
        || strncmp(Config::Database::name, "REPLACE_", 8) == 0
        || strncmp(Config::Database::user, "REPLACE_", 8) == 0
        || strncmp(Config::Database::password, "REPLACE_", 8) == 0) {
        logError(
            "Datenbankzugangsdaten fehlen oder enthalten Platzhalter. "
            "include/Config.h konfigurieren."
        );
        return false;
    }

    logInfo("Verbinde mit MariaDB/MySQL");
    if (!mysqlConnection.connect(
            Config::Database::host,
            Config::Database::port,
            Config::Database::user,
            Config::Database::password,
            Config::Database::name)) {
        logError("Verbindung zum MariaDB/MySQL-Server fehlgeschlagen");
        mysqlConnection.close();
        return false;
    }

    logInfo(
        "Mit MariaDB/MySQL verbunden: "
        + String(Config::Database::host) + ":"
        + String(Config::Database::port) + "/"
        + String(Config::Database::name)
    );
    return true;
}

bool executeInTransaction(const char* statement) {
    MySQL_Query query(&mysqlConnection);
    if (query.execute(statement)) {
        logDebugPlus("SQL-Schritt innerhalb der Datenbanktransaktion erfolgreich");
        return true;
    }
    logError("Datenbank-Transaktion fehlgeschlagen");
    return false;
}

void rollbackAndClose() {
    logWarning("Datenbanktransaktion wird zurueckgerollt");
    if (mysqlConnection.connected()) {
        executeInTransaction("ROLLBACK");
    }
    mysqlConnection.close();
}

bool formatAmount(double amount, char* output, size_t outputSize) {
    if (!isfinite(amount) || amount < 0.0) {
        logError("Messwert ist negativ oder nicht endlich");
        return false;
    }

    const int written = snprintf(output, outputSize, "%.17g", amount);
    if (written < 0 || static_cast<size_t>(written) >= outputSize) {
        logError("Messwert kann nicht formatiert werden");
        return false;
    }
    return true;
}
}

bool DatabaseConnection::connect() {
    logDebug("Expliziter Datenbankverbindungsaufbau angefordert");
    return ensureConnected();
}

bool DatabaseConnection::isConnected() const {
    return mysqlConnection.connected();
}

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

bool DatabaseConnection::addConsumptionBatch(
    const String& deviceIdentifier,
    uint64_t sequence,
    const ConsumptionRecord* records,
    size_t recordCount
) {
    logDebug(
        "Validiere Messintervall; Sequenz "
        + String(static_cast<unsigned long long>(sequence))
        + ", Datensaetze " + String(recordCount)
    );
    if (deviceIdentifier.length() != 12
        || sequence == 0
        || records == nullptr
        || recordCount != expectedCounterCount) {
        logError("Ungueltige Parameter fuer den Messintervall-Upload");
        return false;
    }

    char quotedDeviceId[13];
    for (size_t index = 0; index < 12; ++index) {
        const char value = deviceIdentifier[index];
        if (!isxdigit(static_cast<unsigned char>(value))) {
            logError("Geraete-ID enthaelt ungueltige Zeichen");
            return false;
        }
        quotedDeviceId[index] = value;
    }
    quotedDeviceId[12] = '\0';

    bool seenCounter[expectedCounterCount] = {};
    char formattedAmounts[expectedCounterCount][32];
    for (size_t index = 0; index < recordCount; ++index) {
        const uint8_t counterId = records[index].counterId;
        if (!validCounterId(counterId) || seenCounter[counterId - 4]) {
            logError("Messintervall enthaelt ungueltige oder doppelte Zaehler-IDs");
            return false;
        }
        seenCounter[counterId - 4] = true;
        if (!formatAmount(
                records[index].measuredKWh,
                formattedAmounts[index],
                sizeof(formattedAmounts[index]))) {
            return false;
        }
    }

    if (!ensureConnected()
        || !executeInTransaction("START TRANSACTION")) {
        mysqlConnection.close();
        return false;
    }
    logDebug("Datenbanktransaktion fuer Messintervall gestartet");

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
        if (row == nullptr || row->values[0] == nullptr
            || !parseUint64(row->values[0], lastSequence)) {
            logError("Datenbank-Synchronisationssequenz ist ungueltig");
            rollbackAndClose();
            return false;
        }
    }

    if (sequence <= lastSequence) {
        if (!executeInTransaction("COMMIT")) {
            rollbackAndClose();
            return false;
        }
        mysqlConnection.close();
        logInfo("Messintervall wurde bereits gespeichert; Wiederholung idempotent bestaetigt");
        return true;
    }
    if (lastSequence == UINT64_MAX || sequence != lastSequence + 1) {
        logError("Messintervall-Sequenz ist nicht fortlaufend");
        rollbackAndClose();
        return false;
    }

    if (!executeInTransaction("SET @energy_measurement_time = UTC_TIMESTAMP(6)")) {
        rollbackAndClose();
        return false;
    }

    for (size_t index = 0; index < recordCount; ++index) {
        snprintf(
            statement,
            sizeof(statement),
            "INSERT INTO energy_measurements "
            "(device_id, counter_id, sequence, measured_at, consumption_kwh) "
            "VALUES ('%s', %u, %llu, @energy_measurement_time, %s)",
            quotedDeviceId,
            records[index].counterId,
            static_cast<unsigned long long>(sequence),
            formattedAmounts[index]
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
    logInfo(
        "Messintervall atomar gespeichert; UTC-Zeitstempel durch Datenbank "
        "gesetzt, Sequenz "
        + String(static_cast<unsigned long long>(sequence))
    );
    return true;
}
