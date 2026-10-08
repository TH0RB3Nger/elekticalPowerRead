#include <Arduino.h>

#if __has_include("Config.h")
#include "Config.h"
#else
#error "Config.h fehlt. Kopiere include/Config.h.example nach include/Config.h und trage deine Werte ein."
#endif
#include "ConsumptionSync.h"
#include "CountPin.h"
#include "DatabaseConnection.h"
#include "Logging.h"
#include "NtpClock.h"
#include "NvsStorageMonitor.h"
#include "PersistentConsumptionBuffer.h"
#include "UploadScheduler.h"
#include "WirelessConnection.h"

namespace {
/// Netzwerkzugang fuer die lokale WLAN-Verbindung.
WirelessConnection wifi(
    Config::Wifi::ssid,
    Config::Wifi::password,
    Config::Runtime::wifiConnectionTimeoutMs,
    Config::Runtime::wifiReconnectIntervalMs,
    Config::Runtime::wifiSignalLogIntervalMs
);

/// SQL-Zugriff auf die MariaDB/MySQL-Datenbank.
DatabaseConnection database;

/// Dauerhafter Speicher fuer Verbrauchsdifferenzen bis zur Bestaetigung.
PersistentConsumptionBuffer consumptionBuffer;

/// Koordiniert Datenbankabfrage, Batch-Upload und Flash-Leerung.
ConsumptionSync consumptionSync(
    database,
    consumptionBuffer,
    Config::Runtime::databaseReadRetryMs,
    Config::Runtime::uploadRetryMs
);

/// NTP-Uhr und Mitteleuropa-Sommerzeit.
NtpClock clockSync;

/// Fester lokaler Uploadtakt, standardmaessig alle 15 Minuten.
UploadScheduler uploadScheduler(Config::Runtime::uploadEveryMinutes);

/// Notfall-Uploadueberwachung fuer freie NVS-Eintraege.
NvsStorageMonitor storageMonitor(
    Config::Runtime::minimumFreeNvsPercent,
    Config::Runtime::flashMonitorIntervalMs
);

/// Vier entprellte Impulszaehler fuer GPIO 4 bis 7.
CountPin counter1(4, Config::Runtime::consumptionPerPulse, Config::Runtime::debounceDelayMs);
CountPin counter2(5, Config::Runtime::consumptionPerPulse, Config::Runtime::debounceDelayMs);
CountPin counter3(6, Config::Runtime::consumptionPerPulse, Config::Runtime::debounceDelayMs);
CountPin counter4(7, Config::Runtime::consumptionPerPulse, Config::Runtime::debounceDelayMs);

/// Verhindert, dass ein fehlgeschlagener NVS-Start als gueltiger Puffer gilt.
bool consumptionBufferReady = false;

/**
 * @brief Addiert die seit dem letzten Speichern eingetroffenen Impulse im NVS.
 * @param counter Impulszaehler des GPIO-Pins.
 * @param counterId GPIO-Nummer und Datenbank-ID.
 * @param savedCount Letzter erfolgreich im Flash uebernommener Impulsstand.
 */
void bufferNewPulses(
    CountPin& counter,
    uint8_t counterId,
    unsigned long& savedCount
) {
    if (!consumptionBufferReady) {
        return;
    }

    const unsigned long currentCount = counter.getCount();
    if (currentCount == savedCount) {
        return;
    }

    const unsigned long newPulses = currentCount - savedCount;
    const double addedConsumption =
        newPulses * Config::Runtime::consumptionPerPulse;

    if (consumptionBuffer.addConsumption(counterId, addedConsumption)) {
        savedCount = currentCount;
        logInfo(
            "Verbrauch fuer Zaehler " + String(counterId)
            + " im Flash zwischengespeichert: "
            + String(addedConsumption, 3)
        );
    }
}

/**
 * @brief Startet bei WLAN-Verbindung Uhrzeitabgleich und DB-Zugriff.
 * @param now Aktueller millis()-Zeitwert.
 */
void serviceConnectedNetwork(unsigned long now) {
    clockSync.begin();
    clockSync.logStatus();
    consumptionSync.initialize(now);

    time_t synchronizedTime;
    if (clockSync.getTime(synchronizedTime)) {
        uploadScheduler.update(
            synchronizedTime,
            consumptionBuffer.hasPending()
        );
    } else {
        logDebug("Warte auf NTP-Zeitsynchronisation fuer den Upload-Zeitplan");
    }

    const bool scheduledUpload = uploadScheduler.isDue();
    const bool lowStorageUpload =
        storageMonitor.isCritical() && consumptionBuffer.hasPending();

    if (!consumptionSync.isInitialized()
        || !consumptionBuffer.hasPending()
        || (!scheduledUpload && !lowStorageUpload)
        || !consumptionSync.retryAllowed(now)) {
        return;
    }

    consumptionSync.markUploadAttempt(now);
    if (consumptionSync.uploadPending()) {
        uploadScheduler.markCompleted();
    }
}
}

/**
 * @brief Initialisiert serielle Ausgabe, NVS-Puffer und WLAN.
 */
void setup() {
    Serial.begin(Config::Runtime::serialBaudRate);
    logInfo("System gestartet");

    consumptionBufferReady = consumptionBuffer.begin();
    if (!consumptionBufferReady) {
        logError("Dauerhafter Verbrauchspuffer konnte nicht initialisiert werden");
    }

    if (wifi.connect()) {
        wifi.printStatus();
        clockSync.begin();
    } else {
        logError("WLAN-Verbindung fehlgeschlagen; automatische Wiederverbindung aktiv");
    }
}

/**
 * @brief Sammelt Impulse, prueft WLAN und bedient NTP sowie Datenbank-Sync.
 */
void loop() {
    wifi.processEvents();
    wifi.maintainConnection();
    wifi.printStatus();

    static unsigned long savedCount1 = 0;
    static unsigned long savedCount2 = 0;
    static unsigned long savedCount3 = 0;
    static unsigned long savedCount4 = 0;

    bufferNewPulses(counter1, 4, savedCount1);
    bufferNewPulses(counter2, 5, savedCount2);
    bufferNewPulses(counter3, 6, savedCount3);
    bufferNewPulses(counter4, 7, savedCount4);

    if (consumptionBufferReady) {
        storageMonitor.update(consumptionBuffer, millis());
    }

    if (wifi.isConnected()) {
        serviceConnectedNetwork(millis());
    }

    delay(Config::Runtime::mainLoopDelayMs);
}
