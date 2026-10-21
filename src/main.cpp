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
#include "DatabaseWorker.h"
#include "Logging.h"
#include "NtpClock.h"
#include "NvsStorageMonitor.h"
#include "PulseAccounting.h"
#include "RamStorageMonitor.h"
#include "SerialCommandProcessor.h"
#include "StatusLed.h"
#include "StatusWebServer.h"
#include "UploadScheduler.h"
#include "WirelessConnection.h"

namespace {
double mainLoopLoadPercent = 0.0;

/// Zentrale Betriebszustaende der Erfassung und des sicheren STOP-Ablaufs.
enum class ProgramMode : uint8_t {
    Running,
    Stopping,
    Stopped,
    Killed
};

ProgramMode programMode = ProgramMode::Running;
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

/// Serialisiert Datenbankauftraege auf einem dedizierten FreeRTOS-Task.
DatabaseWorker databaseWorker(database, consumptionSync);

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

/// Uebernimmt Zaehlerdifferenzen und ISR-Diagnosen im Haupttask.
PulseAccounting pulseAccounting(
    consumptionBuffer,
    counter1,
    Config::Counters::databaseId1,
    counter2,
    Config::Counters::databaseId2,
    counter3,
    Config::Counters::databaseId3,
    counter4,
    Config::Counters::databaseId4
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
bool databaseUploadInProgress = false;
bool databaseCheckRequested = false;

/**
 * @brief Berechnet den LED-Zustand in der festgelegten Prioritaetsreihenfolge.
 */
void updateStatusLed() {
    if (programMode == ProgramMode::Stopped
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

/// Uebernimmt Worker-Ergebnisse im Haupttask und aktualisiert Uploadstatus.
void processDatabaseResults() {
    if (!databaseWorker.isActive()) {
        return;
    }

    DatabaseResult result{};
    while (databaseWorker.pollResult(result)) {
        if (result.type == DatabaseCommandType::CheckConnection) {
            databaseReachable = result.succeeded;
            lastDatabaseCheck = millis();
            if (result.succeeded) {
                logInfo("Datenbankverbindung nach WLAN-Aufbau erfolgreich");
            } else {
                logWarning(
                    "WLAN steht, aber die Datenbank ist nicht erreichbar; "
                    "Uploadversuche pruefen die Verbindung erneut"
                );
            }
            updateStatusLed();
            continue;
        }

        databaseUploadInProgress = false;
        uploadFailed = !result.succeeded;
        databaseReachable = result.succeeded;
        lastDatabaseCheck = millis();
        if (result.succeeded) {
            uploadScheduler.markCompleted();
            stopUploadAttempted = false;
            stopNetworkWarningShown = false;
            stopBufferWarningShown = false;
        }
        updateStatusLed();
    }
}

/// Prueft nach einem WLAN-Aufbau die Datenbank, ohne den Mess-Task zu blockieren.
void serviceDatabaseCheck() {
    if (!databaseCheckRequested) {
        return;
    }

    if (!databaseWorker.isActive()) {
        const bool connected = database.checkConnection();
        if (connected) {
            logInfo("Datenbankverbindung nach WLAN-Aufbau erfolgreich");
        } else {
            logWarning(
                "WLAN steht, aber die Datenbank ist nicht erreichbar; "
                "Uploadversuche pruefen die Verbindung erneut"
            );
        }
        databaseReachable = connected;
        lastDatabaseCheck = millis();
        databaseCheckRequested = false;
        return;
    }

    if (databaseWorker.requestConnectionCheck()) {
        databaseCheckRequested = false;
    } else {
        if (!databaseWorker.isBusy()) {
            logError("Datenbankverbindungspruefung konnte nicht eingereiht werden");
        }
    }
}

/// Meldet alle vier GPIO-Interrupts ab, bevor ein letzter Zaehlstand gelesen wird.
void pauseCounterInputs() {
    counter1.pause();
    counter2.pause();
    counter3.pause();
    counter4.pause();
}

/// Registriert die zuvor pausierten Zaehler-Interrupts erneut.
void resumeCounterInputs() {
    counter1.resume();
    counter2.resume();
    counter3.resume();
    counter4.resume();
}

/// Sofort-Callback des Seriell-Tasks; keine Programmzustandsaenderung ausserhalb loop().
void handleImmediateStop(SerialCommandType type, void*) {
    pauseCounterInputs();
    if (type == SerialCommandType::Stop) {
        statusLed.setStopIndicator(StatusLed::StopIndicator::Stopping);
    } else if (type == SerialCommandType::Kill) {
        statusLed.setStatus(StatusLed::Status::Off);
        statusLed.setStopIndicator(StatusLed::StopIndicator::Killed);
    }
}

/// Gibt die im USB-Seriellmonitor verfuegbaren Befehle aus.
void printCommandHelp() {
    Serial.println("Befehle:");
    Serial.println("  STOP                  Erfassung pausieren und Puffer uebertragen");
    Serial.println("  START                 Erfassung wieder starten");
    Serial.println("  KILL                  Sofort stoppen, ohne Puffer-Upload");
    Serial.println("  DEBUG <LEVEL>         ERROR|WARNING|INFO|DEBUG|DEBUG_PLUS");
    Serial.println("  HELP oder ?           Diese Hilfe anzeigen");
}

/// Setzt Befehle im Haupttask um und verwaltet den Programmzustand.
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

    if (databaseWorker.isBusy()) {
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

    if (databaseWorker.isActive()) {
        if (!databaseWorker.requestUpload()) {
            logError("Datenbankupload konnte nicht in die Worker-Queue eingereiht werden");
            uploadFailed = true;
            updateStatusLed();
            return;
        }

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

/// Haelt STOP aktiv, bis alle ausstehenden Werte bestaetigt wurden.
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
    if (databaseWorker.isBusy()) {
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
    if (databaseWorker.isActive()) {
        if (!databaseWorker.requestUpload()) {
            logError("STOP-Upload konnte nicht in die Worker-Queue eingereiht werden");
            uploadFailed = true;
            updateStatusLed();
            return;
        }
        databaseUploadInProgress = true;
        return;
    }

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
        databaseWorker.begin();
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
        databaseCheckRequested = true;
        lastDatabaseCheck = millis();
    }
    const unsigned long now = millis();
    if (captureActive && wifiConnected
        && now - lastDatabaseCheck >= Config::Runtime::databaseCheckIntervalMs) {
        lastDatabaseCheck = now;
        databaseCheckRequested = true;
    }
    wifiWasConnected = wifiConnected;
    if (captureActive && wifiConnected) {
        serviceDatabaseCheck();
    }

    if (programMode == ProgramMode::Running) {
        pulseAccounting.process(consumptionBufferInitialized);
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
        pulseAccounting.flush(consumptionBufferInitialized);
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
