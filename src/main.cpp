#include <Arduino.h>
#include <float.h>

#if __has_include("Config.h")
#include "Config.h"
#else
#error "Config.h fehlt. Kopiere include/Config.h.example nach include/Config.h und trage deine Werte ein."
#endif
#include "ConsumptionBuffer.h"
#include "ConsumptionSync.h"
#include "CountPin.h"
#include "DatabaseConnection.h"
#include "Logging.h"
#include "NtpClock.h"
#include "NvsStorageMonitor.h"
#include "RamStorageMonitor.h"
#include "StatusLed.h"
#include "UploadScheduler.h"
#include "WirelessConnection.h"

namespace {
static_assert(
    Config::Runtime::minimumPulseDurationMs <= 4294967UL
        && Config::Runtime::minimumPulseIntervalMs <= 4294967UL,
    "S0-Zeitparameter muessen in den micros()-Messbereich passen"
);
static_assert(
    Config::Runtime::resolutionPulsesPerKWh > 0.0
        && Config::Runtime::resolutionPulsesPerKWh <= DBL_MAX,
    "Die Zaehleraufloesung muss positiv und endlich sein"
);

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

/// Modusneutraler Speicher fuer Verbrauchsdifferenzen bis zur Bestaetigung.
ConsumptionBuffer consumptionBuffer;

/// Koordiniert Datenbankabfrage, Batch-Upload und Pufferbestaetigung.
ConsumptionSync consumptionSync(
    database,
    consumptionBuffer,
    Config::Runtime::uploadRetryMs
);

/// NTP-Uhr und Mitteleuropa-Sommerzeit.
NtpClock clockSync;

/// Fester lokaler Uploadtakt gemaess Konfiguration.
UploadScheduler uploadScheduler(Config::Runtime::uploadEveryMinutes);

/// Ueberwacht NVS-Freiplatz im NVS-Puffermodus.
NvsStorageMonitor nvsStorageMonitor(
    Config::Runtime::minimumFreeNvsPercent,
    Config::Runtime::nvsMonitorIntervalMs
);

/// Ueberwacht freien Heap im RAM-Puffermodus.
RamStorageMonitor ramStorageMonitor(
    Config::Runtime::minimumFreeRamPercent,
    Config::Runtime::ramMonitorIntervalMs
);

/// Zustandsanzeige ueber die RGB-LED des Arduino Nano ESP32.
StatusLed statusLed;

/// Vier entprellte Impulszaehler an den konfigurierten GPIO-Pins.
CountPin counter1(
    Config::Counters::pin1,
    Config::Runtime::resolutionPulsesPerKWh,
    Config::Runtime::pulseActiveHigh,
    Config::Runtime::minimumPulseDurationMs,
    Config::Runtime::minimumPulseIntervalMs
);
CountPin counter2(
    Config::Counters::pin2,
    Config::Runtime::resolutionPulsesPerKWh,
    Config::Runtime::pulseActiveHigh,
    Config::Runtime::minimumPulseDurationMs,
    Config::Runtime::minimumPulseIntervalMs
);
CountPin counter3(
    Config::Counters::pin3,
    Config::Runtime::resolutionPulsesPerKWh,
    Config::Runtime::pulseActiveHigh,
    Config::Runtime::minimumPulseDurationMs,
    Config::Runtime::minimumPulseIntervalMs
);
CountPin counter4(
    Config::Counters::pin4,
    Config::Runtime::resolutionPulsesPerKWh,
    Config::Runtime::pulseActiveHigh,
    Config::Runtime::minimumPulseDurationMs,
    Config::Runtime::minimumPulseIntervalMs
);

/// Verhindert, dass ein fehlgeschlagener Pufferstart als gueltig gilt.
bool consumptionBufferInitialized = false;

/// Haelt den Fehlerzustand bis zu einem erfolgreichen Upload sichtbar.
bool uploadFailed = false;

/**
 * @brief Berechnet den LED-Zustand in der festgelegten Prioritaetsreihenfolge.
 */
void updateStatusLed() {
    if (!consumptionBufferInitialized || !wifi.isConnected() || uploadFailed) {
        statusLed.setStatus(StatusLed::Status::Error);
    } else if ((Config::Runtime::useNvsConsumptionBuffer
            && nvsStorageMonitor.isCritical())
        || (!Config::Runtime::useNvsConsumptionBuffer
            && ramStorageMonitor.isCritical())) {
        statusLed.setStatus(StatusLed::Status::StorageCritical);
    } else {
        statusLed.setStatus(StatusLed::Status::WifiConnected);
    }
}

/**
 * @brief Meldet ISR-Verwerfungen aus dem Haupttask, niemals direkt aus der ISR.
 * @param counter Zaehler, dessen verworfene Pulse abgefragt werden.
 * @param counterId Feste Datenbank-ID des Zaehlerkanals.
 * @param lastCounts Letzter protokollierter ISR-Diagnosestand.
 */
void logRejectedPulseDiagnostics(
    const CountPin& counter,
    uint8_t counterId,
    RejectedPulseCounts& lastCounts
) {
    const RejectedPulseCounts counts = counter.getRejectedPulseCounts();
    if (counts.tooShort == lastCounts.tooShort
        && counts.tooSoon == lastCounts.tooSoon
        && counts.tooShortSaturated == lastCounts.tooShortSaturated
        && counts.tooSoonSaturated == lastCounts.tooSoonSaturated
        && counts.countSaturated == lastCounts.countSaturated) {
        return;
    }

    logDebugPlus(
        "ISR-Diagnose Zaehler-ID " + String(counterId)
        + ": zu kurze Pulse +" + String(counts.tooShort - lastCounts.tooShort)
        + ", Mindestabstand verletzt +"
        + String(counts.tooSoon - lastCounts.tooSoon)
    );
    if (counts.tooShortSaturated && !lastCounts.tooShortSaturated) {
        logError("ISR-Diagnosezaehler fuer zu kurze Pulse ist ausgeschoepft");
    }
    if (counts.tooSoonSaturated && !lastCounts.tooSoonSaturated) {
        logError("ISR-Diagnosezaehler fuer zu dichte Pulse ist ausgeschoepft");
    }
    if (counts.countSaturated && !lastCounts.countSaturated) {
        logError(
            "Impulszaehler hat UINT64_MAX erreicht; weitere Pulse "
            "koennen nicht mehr erfasst werden"
        );
    }
    lastCounts = counts;
}

/**
 * @brief Addiert neue Impulse zum konfigurierten RAM- oder NVS-Puffer.
 * @param counter Impulszaehler des GPIO-Pins.
 * @param counterId Feste Datenbank-ID des Zaehlerkanals (4 bis 7).
 * @param bufferedCount Letzter bereits in den Verbrauchspuffer uebernommener Stand.
 */
void bufferNewPulses(
    CountPin& counter,
    uint8_t counterId,
    uint64_t& bufferedCount
) {
    if (!consumptionBufferInitialized) {
        return;
    }

    constexpr uint64_t rolloverThreshold = UINT64_MAX - 1000000ULL;
    const uint64_t currentCount = counter.getCount();
    if (currentCount < bufferedCount) {
        logError(
            "Impulszaehler fuer ID " + String(counterId)
            + " wurde zurueckgesetzt; passe den gespeicherten Stand an"
        );
        bufferedCount = currentCount;
        return;
    }

    if (currentCount != bufferedCount) {
        const uint64_t newPulses = currentCount - bufferedCount;
        const double addedConsumption =
            newPulses / Config::Runtime::resolutionPulsesPerKWh;
        if (!isfinite(addedConsumption)) {
            logError(
                "Verbrauchsdifferenz fuer Zaehler-ID " + String(counterId)
                + " ist ausserhalb des darstellbaren Zahlenbereichs"
            );
            return;
        }

        logDebug(
            "Neue gueltige Pulse fuer Zaehler-ID " + String(counterId)
            + ": " + String(static_cast<unsigned long long>(newPulses))
            + " (" + String(addedConsumption, 6) + " kWh)"
        );
        if (!consumptionBuffer.addConsumption(counterId, addedConsumption)) {
            return;
        }
        bufferedCount = currentCount;
        logDebug(
            "Verbrauch fuer Zaehler-ID " + String(counterId)
            + " im konfigurierten Puffer uebernommen"
        );
    }

    if (currentCount < rolloverThreshold || bufferedCount != currentCount) {
        return;
    }
    if (!consumptionBuffer.requestImmediateUpload()) {
        return;
    }
    if (!counter.subtractCount(currentCount)) {
        logError(
            "Impulszaehler fuer ID " + String(counterId)
            + " konnte wegen einer Saettigung nicht sicher entlastet werden"
        );
        return;
    }

    bufferedCount = 0;
    logWarning(
        "Impulszaehler fuer ID " + String(counterId)
        + " nahe am Zahlenlimit; gepufferter Stand wird sofort uebertragen"
    );
}

/**
 * @brief Startet bei WLAN-Verbindung Uhrzeitabgleich und DB-Zugriff.
 * @param now Aktueller millis()-Zeitwert.
 */
void serviceConnectedNetwork(unsigned long now) {
    static bool immediateRequestAttempted = false;

    clockSync.begin();
    clockSync.logStatus();
    time_t synchronizedTime;
    if (clockSync.getTime(synchronizedTime)) {
        uploadScheduler.update(synchronizedTime);
    }

    const bool scheduledUpload = uploadScheduler.isDue();
    const bool immediateUpload =
        consumptionBuffer.isImmediateUploadRequested();
    if (!immediateUpload) {
        immediateRequestAttempted = false;
    }
    const bool lowStorageUpload = Config::Runtime::useNvsConsumptionBuffer
        && nvsStorageMonitor.isCritical()
        && consumptionBuffer.hasPending();

    if (!scheduledUpload && !lowStorageUpload && !immediateUpload) {
        return;
    }
    if (!consumptionSync.retryAllowed(now)
        && (!immediateUpload || immediateRequestAttempted)) {
        return;
    }

    if (lowStorageUpload && !scheduledUpload) {
        logWarning("NVS-Schwelle erreicht; vorgezogener Upload wird versucht");
    }
    if (immediateUpload) {
        immediateRequestAttempted = true;
        logWarning("Dauerhaft vorgemerkter Sofort-Upload wird ausgefuehrt");
    }

    consumptionSync.markUploadAttempt(now);
    uploadFailed = false;
    if (consumptionBufferInitialized) {
        statusLed.setStatus(StatusLed::Status::Uploading);
    } else {
        updateStatusLed();
    }

    const bool uploadSucceeded = consumptionSync.uploadPending();
    uploadFailed = !uploadSucceeded;
    updateStatusLed();
    if (uploadSucceeded) {
        immediateRequestAttempted = false;
        uploadScheduler.markCompleted();
    }
}
}

/**
 * @brief Initialisiert serielle Ausgabe, Verbrauchspuffer und WLAN.
 */
void setup() {
    Serial.begin(Config::Runtime::serialBaudRate);
    statusLed.begin();
    logInfo("System gestartet");
    logInfo(
        "Konfiguration: Aufloesung "
        + String(Config::Runtime::resolutionPulsesPerKWh, 3)
        + " Impulse/kWh, Upload alle "
        + String(Config::Runtime::uploadEveryMinutes) + " min"
    );
    logInfo(
        String("Verbrauchspuffer: ")
        + (Config::Runtime::useNvsConsumptionBuffer
            ? "NVS (Impulse dauerhaft gespeichert)"
            : "RAM (Puffer geht bei Stromverlust verloren)")
    );
    logDebug(
        "GPIO-Zuordnung: Zaehler 1..4 = "
        + String(Config::Counters::pin1) + ", "
        + String(Config::Counters::pin2) + ", "
        + String(Config::Counters::pin3) + ", "
        + String(Config::Counters::pin4)
    );
    logDebug(
        "S0-Eingang: aktiv "
        + String(Config::Runtime::pulseActiveHigh ? "HIGH" : "LOW")
        + ", Mindestdauer "
        + String(Config::Runtime::minimumPulseDurationMs)
        + " ms, Mindestabstand "
        + String(Config::Runtime::minimumPulseIntervalMs) + " ms"
    );
    if (Config::Runtime::useNvsConsumptionBuffer) {
        logDebug(
            "NVS-Ueberwachung: Schwelle "
            + String(Config::Runtime::minimumFreeNvsPercent)
            + " %, Intervall "
            + String(Config::Runtime::nvsMonitorIntervalMs) + " ms"
        );
    } else {
        logDebug(
            "RAM-Heap-Ueberwachung: Mindestreserve "
            + String(Config::Runtime::minimumFreeRamPercent)
            + " %, Intervall "
            + String(Config::Runtime::ramMonitorIntervalMs) + " ms"
        );
    }
    logDebug(
        "Datenbank-Upload-Wiederholungsintervall: "
        + String(Config::Runtime::uploadRetryMs) + " ms"
    );

    consumptionBufferInitialized = consumptionBuffer.begin();
    if (!consumptionBufferInitialized) {
        logError("Verbrauchspuffer konnte nicht initialisiert werden");
    }

    if (wifi.connect()) {
        wifi.printStatus();
        clockSync.begin();
    } else {
        logError("WLAN-Verbindung fehlgeschlagen; automatische Wiederverbindung aktiv");
    }
    updateStatusLed();
}

/**
 * @brief Sammelt Impulse, prueft WLAN und bedient NTP sowie Datenbank-Sync.
 */
void loop() {
    static RejectedPulseCounts rejected1{};
    static RejectedPulseCounts rejected2{};
    static RejectedPulseCounts rejected3{};
    static RejectedPulseCounts rejected4{};

    wifi.processEvents();
    wifi.maintainConnection();
    wifi.printStatus();

    static uint64_t bufferedCount1 = 0;
    static uint64_t bufferedCount2 = 0;
    static uint64_t bufferedCount3 = 0;
    static uint64_t bufferedCount4 = 0;

    bufferNewPulses(counter1, 4, bufferedCount1);
    bufferNewPulses(counter2, 5, bufferedCount2);
    bufferNewPulses(counter3, 6, bufferedCount3);
    bufferNewPulses(counter4, 7, bufferedCount4);

    logRejectedPulseDiagnostics(counter1, 4, rejected1);
    logRejectedPulseDiagnostics(counter2, 5, rejected2);
    logRejectedPulseDiagnostics(counter3, 6, rejected3);
    logRejectedPulseDiagnostics(counter4, 7, rejected4);

    if (consumptionBufferInitialized) {
        const unsigned long now = millis();
        if (Config::Runtime::useNvsConsumptionBuffer) {
            nvsStorageMonitor.update(now);
        } else {
            ramStorageMonitor.update(now);
        }
    }

    if (wifi.isConnected()) {
        serviceConnectedNetwork(millis());
    }

    updateStatusLed();
    delay(Config::Runtime::mainLoopDelayMs);
}
