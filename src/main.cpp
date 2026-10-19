#include <Arduino.h>
#include <float.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

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
#include "SerialCommandProcessor.h"
#include "StatusLed.h"
#include "StatusWebServer.h"
#include "UploadScheduler.h"
#include "WirelessConnection.h"

namespace {
enum class DatabaseCommandType : uint8_t {
    CheckConnection,
    UploadPending
};

struct DatabaseCommand {
    DatabaseCommandType type;
};

struct DatabaseResult {
    DatabaseCommandType type;
    bool succeeded;
};

double mainLoopLoadPercent = 0.0;

enum class ProgramMode : uint8_t {
    Running,
    Stopping,
    Stopped,
    Killed
};

ProgramMode programMode = ProgramMode::Running;
volatile bool killSignal = false;
bool stopRequested = false;
bool stopUploadAttempted = false;
bool stopNetworkWarningShown = false;
bool stopBufferWarningShown = false;
unsigned long lastStopUploadAttempt = 0;

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
static_assert(
    Config::Counters::pin1 != LED_BUILTIN
        && Config::Counters::pin2 != LED_BUILTIN
        && Config::Counters::pin3 != LED_BUILTIN
        && Config::Counters::pin4 != LED_BUILTIN,
    "LED_BUILTIN muss fuer die orange Status-LED frei bleiben"
);

/// Netzwerkzugang fuer die lokale WLAN-Verbindung.
WirelessConnection wirelessConnection(
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

/// Lokale Weboberflaeche fuer WLAN-, Puffer-, RAM- und Loop-Status.
StatusWebServer statusWebServer(wirelessConnection, consumptionBuffer);

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

/// Nicht-blockierender Parser fuer USB-Seriellbefehle.
SerialCommandProcessor serialCommandProcessor;

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
bool databaseReachable = false;
unsigned long lastDatabaseCheck = 0;

/// Erkennt WLAN-Verbindungsaufbauten fuer einen einmaligen DB-Verbindungstest.
bool wifiWasConnected = false;

/// Uebergibt blockierende Datenbankarbeit an einen Task auf Core 0.
QueueHandle_t databaseCommandQueue = nullptr;
QueueHandle_t databaseResultQueue = nullptr;
bool databaseWorkerActive = false;
bool databaseWorkerBusy = false;
bool databaseUploadInProgress = false;
bool databaseCheckRequested = false;

/**
 * @brief Berechnet den LED-Zustand in der festgelegten Prioritaetsreihenfolge.
 */
void updateStatusLed() {
    if (killSignal || programMode == ProgramMode::Stopped
        || programMode == ProgramMode::Killed) {
        statusLed.setStatus(StatusLed::Status::Off);
        return;
    }
    if (!consumptionBufferInitialized
        || !wirelessConnection.isConnected()
        || uploadFailed) {
        statusLed.setStatus(StatusLed::Status::Error);
    } else if (databaseUploadInProgress) {
        statusLed.setStatus(StatusLed::Status::Uploading);
    } else if ((Config::Runtime::useNvsConsumptionBuffer
            && nvsStorageMonitor.isCritical())
        || (!Config::Runtime::useNvsConsumptionBuffer
            && ramStorageMonitor.isCritical())) {
        statusLed.setStatus(StatusLed::Status::StorageCritical);
    } else if (!databaseReachable) {
        statusLed.setStatus(StatusLed::Status::DatabaseUnavailable);
    } else {
        statusLed.setStatus(StatusLed::Status::WifiConnected);
    }
}

/// Fuehrt ausschliesslich Datenbankoperationen aus dem Worker-Task aus.
void databaseWorkerTask(void*) {
    DatabaseCommand command{};
    while (true) {
        if (xQueueReceive(
                databaseCommandQueue,
                &command,
                portMAX_DELAY) != pdPASS) {
            logError("Datenbank-Worker konnte keinen Auftrag aus der Queue lesen");
            continue;
        }

        const bool succeeded =
            command.type == DatabaseCommandType::CheckConnection
                ? database.checkConnection()
                : consumptionSync.uploadPending();
        const DatabaseResult result{command.type, succeeded};
        if (xQueueSend(databaseResultQueue, &result, portMAX_DELAY) != pdPASS) {
            logError("Datenbank-Worker konnte kein Ergebnis zurueckmelden");
        }
    }
}

/// Startet den Datenbank-Worker auf dem von der Messschleife getrennten Core.
bool startDatabaseWorker() {
    databaseCommandQueue = xQueueCreate(1, sizeof(DatabaseCommand));
    databaseResultQueue = xQueueCreate(1, sizeof(DatabaseResult));
    if (databaseCommandQueue == nullptr || databaseResultQueue == nullptr) {
        if (databaseCommandQueue != nullptr) {
            vQueueDelete(databaseCommandQueue);
            databaseCommandQueue = nullptr;
        }
        if (databaseResultQueue != nullptr) {
            vQueueDelete(databaseResultQueue);
            databaseResultQueue = nullptr;
        }
        logError("Queues fuer den Datenbank-Worker konnten nicht angelegt werden");
        return false;
    }

    if (xTaskCreatePinnedToCore(
            databaseWorkerTask,
            "database_worker",
            8192,
            nullptr,
            1,
            nullptr,
            0) != pdPASS) {
        vQueueDelete(databaseCommandQueue);
        vQueueDelete(databaseResultQueue);
        databaseCommandQueue = nullptr;
        databaseResultQueue = nullptr;
        logError("Datenbank-Worker auf Core 0 konnte nicht gestartet werden");
        return false;
    }

    databaseWorkerActive = true;
    logInfo("Datenbank-Worker auf Core 0 gestartet; Messschleife bleibt entkoppelt");
    return true;
}

/// Uebernimmt Worker-Ergebnisse im Haupttask und aktualisiert Uploadstatus.
void processDatabaseResults() {
    if (!databaseWorkerActive) {
        return;
    }

    DatabaseResult result{};
    while (xQueueReceive(databaseResultQueue, &result, 0) == pdPASS) {
        databaseWorkerBusy = false;
        if (result.type == DatabaseCommandType::CheckConnection) {
            if (result.succeeded) {
                logInfo("Datenbankverbindung nach WLAN-Aufbau erfolgreich");
            } else {
                logWarning(
                    "WLAN steht, aber die Datenbank ist nicht erreichbar; "
                    "Uploadversuche pruefen die Verbindung erneut"
                );
            }
            continue;
        }

        databaseUploadInProgress = false;
        uploadFailed = !result.succeeded;
        if (result.succeeded) {
            uploadScheduler.markCompleted();
        }
        updateStatusLed();
    }
}

/// Prueft nach einem WLAN-Aufbau die Datenbank, ohne den Mess-Task zu blockieren.
void serviceDatabaseCheck() {
    if (!databaseCheckRequested) {
        return;
    }

    if (!databaseWorkerActive) {
        const bool connected = database.checkConnection();
        if (connected) {
            logInfo("Datenbankverbindung nach WLAN-Aufbau erfolgreich");
        } else {
            logWarning(
                "WLAN steht, aber die Datenbank ist nicht erreichbar; "
                "Uploadversuche pruefen die Verbindung erneut"
            );
        }
        databaseCheckRequested = false;
        return;
    }

    if (databaseWorkerBusy) {
        return;
    }
    const DatabaseCommand command{DatabaseCommandType::CheckConnection};
    if (xQueueSend(databaseCommandQueue, &command, 0) == pdPASS) {
        databaseWorkerBusy = true;
        databaseCheckRequested = false;
    } else {
        logError("Datenbankverbindungspruefung konnte nicht eingereiht werden");
    }
}

void pauseCounterInputs() {
    counter1.pause();
    counter2.pause();
    counter3.pause();
    counter4.pause();
}

void resumeCounterInputs() {
    counter1.resume();
    counter2.resume();
    counter3.resume();
    counter4.resume();
}

void handleImmediateStop(SerialCommandType type, void*) {
    pauseCounterInputs();
    if (type == SerialCommandType::Stop) {
        statusLed.setStopIndicator(StatusLed::StopIndicator::Stopping);
    } else if (type == SerialCommandType::Kill) {
        killSignal = true;
        statusLed.setStatus(StatusLed::Status::Off);
        statusLed.setStopIndicator(StatusLed::StopIndicator::Killed);
    }
}

void printCommandHelp() {
    Serial.println("Befehle:");
    Serial.println("  STOP                  Erfassung pausieren und Puffer uebertragen");
    Serial.println("  START                 Erfassung wieder starten");
    Serial.println("  KILL                  Sofort stoppen, ohne Puffer-Upload");
    Serial.println("  DEBUG <LEVEL>         ERROR|WARNING|INFO|DEBUG|DEBUG_PLUS");
    Serial.println("  HELP oder ?           Diese Hilfe anzeigen");
}

void handleSerialCommand(const SerialCommand& command) {
    switch (command.type) {
        case SerialCommandType::Help:
            printCommandHelp();
            break;
        case SerialCommandType::Stop:
            if (programMode == ProgramMode::Running) {
                stopRequested = true;
                Serial.println("STOP angefordert; letzter Zaehlstand wird gesichert.");
            } else {
                Serial.println("STOP nicht moeglich: Erfassung ist bereits angehalten.");
            }
            break;
        case SerialCommandType::Start:
            if (programMode == ProgramMode::Running) {
                if (stopRequested) {
                    stopRequested = false;
                    resumeCounterInputs();
                    statusLed.setStopIndicator(StatusLed::StopIndicator::Off);
                    Serial.println("STOP abgebrochen; Erfassung laeuft weiter.");
                    break;
                }
                Serial.println("Erfassung laeuft bereits.");
                break;
            }
            resumeCounterInputs();
            killSignal = false;
            stopRequested = false;
            stopUploadAttempted = false;
            stopNetworkWarningShown = false;
            stopBufferWarningShown = false;
            programMode = ProgramMode::Running;
            statusLed.setStopIndicator(StatusLed::StopIndicator::Off);
            updateStatusLed();
            Serial.println("Erfassung gestartet.");
            break;
        case SerialCommandType::Kill:
            pauseCounterInputs();
            killSignal = true;
            stopRequested = false;
            programMode = ProgramMode::Killed;
            statusLed.setStatus(StatusLed::Status::Off);
            statusLed.setStopIndicator(StatusLed::StopIndicator::Killed);
            Serial.println(
                "KILL verarbeitet; ungepufferte Impulse und "
                "nicht uebertragene RAM-Werte koennen verloren gehen."
            );
            break;
        case SerialCommandType::SetLogLevel:
            setLogLevel(command.logLevel);
            Serial.print("Log-Level: ");
            Serial.println(getLogLevelName(getLogLevel()));
            break;
        case SerialCommandType::Invalid:
            Serial.println("Unbekannter oder ungueltiger Befehl. HELP zeigt die Befehle.");
            break;
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

    if (databaseWorkerActive && databaseWorkerBusy) {
        return;
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

    if (databaseWorkerActive) {
        const DatabaseCommand command{DatabaseCommandType::UploadPending};
        if (xQueueSend(databaseCommandQueue, &command, 0) != pdPASS) {
            logError("Datenbankupload konnte nicht in die Worker-Queue eingereiht werden");
            uploadFailed = true;
            updateStatusLed();
            return;
        }

        databaseWorkerBusy = true;
        databaseUploadInProgress = true;
        return;
    }

    const bool uploadSucceeded = consumptionSync.uploadPending();
    uploadFailed = !uploadSucceeded;
    databaseReachable = uploadSucceeded;
    lastDatabaseCheck = millis();
    updateStatusLed();
    if (uploadSucceeded) {
        immediateRequestAttempted = false;
        uploadScheduler.markCompleted();
    }
}

void serviceStop(unsigned long now) {
    if (!consumptionBufferInitialized) {
        updateStatusLed();
        if (!stopBufferWarningShown) {
            Serial.println("STOP kann nicht bestaetigt werden: Verbrauchspuffer fehlt.");
            stopBufferWarningShown = true;
        }
        return;
    }
    if (!consumptionBuffer.hasPending()
        && !consumptionBuffer.isImmediateUploadRequested()) {
        programMode = ProgramMode::Stopped;
        statusLed.setStatus(StatusLed::Status::Off);
        statusLed.setStopIndicator(StatusLed::StopIndicator::Stopped);
        Serial.println("STOP abgeschlossen: Puffer leer; Flashen ist sicher.");
        return;
    }

    if (!wirelessConnection.isConnected()) {
        if (!stopNetworkWarningShown) {
            Serial.println("STOP wartet auf WLAN; Orange blinkt, Puffer bleibt erhalten.");
            stopNetworkWarningShown = true;
        }
        updateStatusLed();
        return;
    }
    if (stopUploadAttempted
        && now - lastStopUploadAttempt < Config::Runtime::uploadRetryMs) {
        return;
    }

    stopUploadAttempted = true;
    lastStopUploadAttempt = now;
    consumptionSync.markUploadAttempt(now);
    uploadFailed = false;
    statusLed.setStatus(StatusLed::Status::Uploading);
    Serial.println("STOP: uebertrage ausstehende Messwerte.");
    const bool uploadSucceeded = consumptionSync.uploadPending();
    uploadFailed = !uploadSucceeded;
    databaseReachable = uploadSucceeded;
    lastDatabaseCheck = millis();
    updateStatusLed();
    if (uploadSucceeded) {
        stopUploadAttempted = false;
        stopNetworkWarningShown = false;
        stopBufferWarningShown = false;
    }
}
}

/**
 * @brief Initialisiert serielle Ausgabe, Verbrauchspuffer und WLAN.
 */
void setup() {
    Serial.begin(Config::Runtime::serialBaudRate);
    const bool loggingSynchronized = beginLogging();
    if (!loggingSynchronized) {
        logError("Logger-Mutex konnte nicht erstellt werden; Datenbank-Worker bleibt aus");
    }
    statusLed.begin();
    if (!serialCommandProcessor.begin(handleImmediateStop, nullptr)) {
        logError("Serieller Befehlstask konnte nicht gestartet werden");
    }
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

    if (loggingSynchronized) {
        startDatabaseWorker();
    }

    if (loggingSynchronized) {
        startDatabaseWorker();
    }

    if (wirelessConnection.connect()) {
        wirelessConnection.printStatus();
        clockSync.begin();
    } else {
        logError("WLAN-Verbindung fehlgeschlagen; automatische Wiederverbindung aktiv");
    }
    statusWebServer.begin();
    updateStatusLed();
}

/**
 * @brief Sammelt Impulse und bedient WLAN; DB-Zugriffe laufen im Worker-Task.
 */
void loop() {
    const unsigned long loopStartedAt = micros();
    SerialCommand command;
    while (serialCommandProcessor.poll(command)) {
        handleSerialCommand(command);
    }

    static RejectedPulseCounts rejected1{};
    static RejectedPulseCounts rejected2{};
    static RejectedPulseCounts rejected3{};
    static RejectedPulseCounts rejected4{};

    processDatabaseResults();
    const bool captureActive = programMode == ProgramMode::Running
        || programMode == ProgramMode::Stopping;
    if (captureActive) {
        wirelessConnection.processEvents();
        wirelessConnection.maintainConnection();
        wirelessConnection.printStatus();
    }

    const bool wifiConnected = wirelessConnection.isConnected();
    if (!wifiConnected) {
        databaseReachable = false;
    }
    if (captureActive && wifiConnected && !wifiWasConnected) {
        statusWebServer.logAddress();
        databaseReachable = database.checkConnection();
        lastDatabaseCheck = millis();
        if (databaseReachable) {
            logInfo("Datenbankverbindung nach WLAN-Aufbau erfolgreich");
        } else {
            logWarning(
                "WLAN steht, aber die Datenbank ist nicht erreichbar; "
                "Uploadversuche pruefen die Verbindung erneut"
            );
        }
    }
    const unsigned long now = millis();
    if (captureActive && wifiConnected
        && now - lastDatabaseCheck >= Config::Runtime::databaseCheckIntervalMs) {
        const bool wasReachable = databaseReachable;
        lastDatabaseCheck = now;
        databaseReachable = database.checkConnection();
        if (databaseReachable != wasReachable) {
            logInfo(databaseReachable
                ? "Datenbank wieder erreichbar"
                : "Datenbank nicht erreichbar; RGB-LED zeigt Blau");
        }
    }
    wifiWasConnected = wifiConnected;
    serviceDatabaseCheck();

    static uint64_t bufferedCount1 = 0;
    static uint64_t bufferedCount2 = 0;
    static uint64_t bufferedCount3 = 0;
    static uint64_t bufferedCount4 = 0;

    if (programMode == ProgramMode::Running) {
        bufferNewPulses(counter1, Config::Counters::databaseId1, bufferedCount1);
        bufferNewPulses(counter2, Config::Counters::databaseId2, bufferedCount2);
        bufferNewPulses(counter3, Config::Counters::databaseId3, bufferedCount3);
        bufferNewPulses(counter4, Config::Counters::databaseId4, bufferedCount4);

        logRejectedPulseDiagnostics(counter1, Config::Counters::databaseId1, rejected1);
        logRejectedPulseDiagnostics(counter2, Config::Counters::databaseId2, rejected2);
        logRejectedPulseDiagnostics(counter3, Config::Counters::databaseId3, rejected3);
        logRejectedPulseDiagnostics(counter4, Config::Counters::databaseId4, rejected4);
    }

    if (consumptionBufferInitialized
        && (!Config::Runtime::useNvsConsumptionBuffer
            || !databaseUploadInProgress)) {
        const unsigned long now = millis();
        if (Config::Runtime::useNvsConsumptionBuffer) {
            nvsStorageMonitor.update(now);
        } else {
            ramStorageMonitor.update(now);
        }
    }

    if (stopRequested && programMode == ProgramMode::Running) {
        pauseCounterInputs();
        bufferNewPulses(counter1, Config::Counters::databaseId1, bufferedCount1);
        bufferNewPulses(counter2, Config::Counters::databaseId2, bufferedCount2);
        bufferNewPulses(counter3, Config::Counters::databaseId3, bufferedCount3);
        bufferNewPulses(counter4, Config::Counters::databaseId4, bufferedCount4);
        stopRequested = false;
        programMode = ProgramMode::Stopping;
        stopUploadAttempted = false;
        stopNetworkWarningShown = false;
        statusLed.setStopIndicator(StatusLed::StopIndicator::Stopping);
        Serial.println("STOP aktiv: Erfassung pausiert; sichere Uebertragung beginnt.");
    }

    if (programMode == ProgramMode::Running && wifiConnected) {
        serviceConnectedNetwork(millis());
    } else if (programMode == ProgramMode::Stopping) {
        serviceStop(millis());
    }

    if (programMode == ProgramMode::Running
        || programMode == ProgramMode::Stopping) {
        updateStatusLed();
    }
    statusWebServer.handleClient();
    const unsigned long activeMicros = micros() - loopStartedAt;
    const unsigned long delayedMicros =
        Config::Runtime::mainLoopDelayMs * 1000UL;
    mainLoopLoadPercent = static_cast<double>(activeMicros) * 100.0
        / (activeMicros + delayedMicros);
    statusWebServer.setLoopLoadPercent(mainLoopLoadPercent);
    delay(programMode == ProgramMode::Running
        ? Config::Runtime::mainLoopDelayMs
        : 50UL);
}
