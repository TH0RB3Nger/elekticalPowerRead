#include <Arduino.h>
#include <time.h>

#include "CountPin.h"
#include "DatabaseConnection.h"
#include "Logging.h"
#include "PersistentConsumptionBuffer.h"
#include "WirelessConnection.h"

namespace {
/// Entprellzeit der Impulse in Millisekunden.
constexpr unsigned long debounceDelay = 50;

/// Umrechnungsfaktor pro erkanntem Impuls.
constexpr double counterFactor = 100.0;

/// Uploads erfolgen an den Viertelstunden der lokalen Uhrzeit.
constexpr uint8_t uploadEveryMinutes = 15;

/// Bei diesem oder kleinerem freien NVS-Anteil wird sofort ein Upload versucht.
constexpr uint8_t minimumFreeNvsPercent = 10;

/// Maximaler Abstand zwischen Wiederholungen eines fehlgeschlagenen Uploads.
constexpr unsigned long uploadRetryMs = 30000UL;

/// Mindestabstand zwischen Flash-Statistikmeldungen.
constexpr unsigned long flashMonitorIntervalMs = 60000UL;

/// Mindestabstand zwischen Versuchen, die Datenbank beim Start abzufragen.
constexpr unsigned long databaseReadRetryMs = 30000UL;

/// POSIX-Zeitzone fuer Mitteleuropa mit automatischer Sommerzeit.
constexpr char timeZone[] = "CET-1CEST,M3.5.0/2,M10.5.0/3";

/// NTP-Server fuer die Uhrzeitsynchronisation.
constexpr char ntpServer1[] = "pool.ntp.org";
constexpr char ntpServer2[] = "time.nist.gov";

static_assert(
    uploadEveryMinutes > 0 && 60 % uploadEveryMinutes == 0,
    "uploadEveryMinutes muss ein positiver Teiler von 60 sein"
);

/// WLAN-Verwaltung mit den lokalen Zugangsdaten.
WirelessConnection wifi("MeinWLAN", "MeinPasswort");

/// Verbindung und CRUD-Zugriff auf die Verbrauchsdatenbank.
DatabaseConnection database;

/// Dauerhafter Flash-Puffer fuer noch nicht bestaetigte Verbrauchswerte.
PersistentConsumptionBuffer consumptionBuffer;

/// Impulszaehler fuer die GPIO-Pins 4 bis 7.
CountPin counter1(4, counterFactor, debounceDelay);
CountPin counter2(5, counterFactor, debounceDelay);
CountPin counter3(6, counterFactor, debounceDelay);
CountPin counter4(7, counterFactor, debounceDelay);

/// Letzter vom Datenbankserver bestaetigter Gesamtstand pro Zaehler.
bool databaseInitialized = false;

/// Zeitpunkt des letzten Versuchs, die Datenbankstaende zu lesen.
unsigned long lastDatabaseReadAttempt = 0;

/// Zeitpunkt der letzten NVS-Kapazitaetspruefung.
unsigned long lastFlashMonitor = 0;

/// Letzter NVS-Freianteil, der fuer die Uploadentscheidung verwendet wurde.
uint8_t freeNvsPercent = 100;

/// Status fuer die periodische NVS-Diagnose und den Notfall-Upload.
bool flashStatsAvailable = false;
bool flashThresholdReached = false;

/// Status der NTP-Zeitsynchronisation.
bool ntpConfigured = false;
bool ntpReadyLogged = false;

/// Zeitplanstatus fuer den aktuellen epochbasierten Viertelstunden-Slot.
time_t observedUploadSlot = -1;
bool scheduledUploadDue = false;

/// Zeit des letzten Datenbank-Schreibversuchs fuer begrenzte Wiederholungen.
unsigned long lastUploadAttempt = 0;
bool uploadAttempted = false;

/**
 * @brief Uebernimmt neue Impulse in den Flash-Puffer.
 * @param counter Der auszulesende Pinzaehler.
 * @param counterId Die GPIO-Nummer und Datenbank-ID des Zaehlerkanals.
 * @param savedCount Der zuletzt dauerhaft gepufferte Impulsstand.
 */
void bufferNewPulses(
    CountPin& counter,
    uint8_t counterId,
    unsigned long& savedCount
) {
    const unsigned long currentCount = counter.getCount();
    if (currentCount == savedCount) {
        return;
    }

    const unsigned long newPulses = currentCount - savedCount;
    const double addedConsumption = newPulses * counterFactor;

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
 * @brief Liest die gespeicherten Gesamtstaende vor dem ersten Upload.
 * @param now Aktueller millis()-Zeitwert.
 */
void initializeDatabaseState(unsigned long now) {
    if (databaseInitialized
        || now - lastDatabaseReadAttempt < databaseReadRetryMs) {
        return;
    }

    lastDatabaseReadAttempt = now;
    ConsumptionRecord records[4];
    size_t recordCount = 0;
    if (!database.readAll(records, 4, recordCount)) {
        logWarning("Datenbankstaende konnten noch nicht gelesen werden");
        return;
    }

    if (recordCount != 4) {
        logError(
            "Die Datenbank muss genau vier Zaehlerdatensaetze "
            "(GPIO 4 bis 7) enthalten"
        );
        return;
    }

    for (size_t index = 0; index < recordCount; ++index) {
        logInfo(
            "Datenbankstand Zaehler " + String(records[index].counterId)
            + ": " + String(records[index].totalConsumption, 3)
        );
    }
    databaseInitialized = true;
}

/**
 * @brief Initialisiert SNTP fuer die lokale Mitteleuropa-Zeit.
 */
void configureNtp() {
    if (ntpConfigured) {
        return;
    }

    configTzTime(timeZone, ntpServer1, ntpServer2);
    ntpConfigured = true;
    logInfo("NTP gestartet; Zeitzone Europe/Berlin mit Sommerzeit aktiviert");
}

/**
 * @brief Prueft, ob die Systemuhr bereits eine plausible NTP-Zeit enthaelt.
 * @param currentTime Gibt bei Erfolg die aktuelle Unix-Zeit zurueck.
 * @return true, wenn die Uhrzeit mit NTP synchronisiert ist.
 */
bool getSynchronizedTime(time_t& currentTime) {
    currentTime = time(nullptr);
    constexpr time_t plausibleEpoch = 1700000000;
    return currentTime >= plausibleEpoch;
}

/**
 * @brief Protokolliert eine erfolgreiche NTP-Synchronisation genau einmal.
 */
void logNtpStatus() {
    if (ntpReadyLogged) {
        return;
    }

    time_t now;
    if (!getSynchronizedTime(now)) {
        return;
    }

    struct tm localTime;
    if (localtime_r(&now, &localTime) == nullptr) {
        logError("Lokale Zeit konnte nach NTP-Synchronisation nicht gelesen werden");
        return;
    }

    char formattedTime[32];
    if (strftime(
            formattedTime,
            sizeof(formattedTime),
            "%Y-%m-%d %H:%M:%S %Z",
            &localTime) == 0) {
        logError("NTP-Zeit konnte nicht formatiert werden");
        return;
    }

    ntpReadyLogged = true;
    logInfo("NTP-Zeit synchronisiert: " + String(formattedTime));
}

/**
 * @brief Ermittelt den freien NVS-Anteil und aktiviert bei Bedarf den Notfall-Upload.
 * @param now Aktueller millis()-Zeitwert.
 */
void monitorFlash(unsigned long now) {
    if (lastFlashMonitor != 0
        && now - lastFlashMonitor < flashMonitorIntervalMs) {
        return;
    }
    lastFlashMonitor = now;

    uint32_t freeEntries = 0;
    uint32_t totalEntries = 0;
    uint8_t currentFreePercent = 0;
    if (!consumptionBuffer.getStorageStats(
            freeEntries,
            totalEntries,
            currentFreePercent)) {
        flashStatsAvailable = false;
        flashThresholdReached = false;
        return;
    }

    flashStatsAvailable = true;
    freeNvsPercent = currentFreePercent;
    flashThresholdReached =
        static_cast<uint64_t>(freeEntries) * 100ULL
        <= static_cast<uint64_t>(totalEntries) * minimumFreeNvsPercent;

    logInfo(
        "NVS frei: " + String(freeEntries) + "/" + String(totalEntries)
        + " Eintraege (" + String(freeNvsPercent) + " %)"
    );
    if (flashThresholdReached) {
        logWarning(
            "NVS-Freianteil bei oder unter "
            + String(minimumFreeNvsPercent)
            + " %; zusaetzlicher Upload wird ausgeloest"
        );
    }
}

/**
 * @brief Erkennt den Wechsel in einen neuen lokalen Viertelstunden-Slot.
 * @param currentTime Synchronisierte Unix-Zeit.
 */
void updateUploadSchedule(time_t currentTime) {
    const time_t intervalSeconds =
        static_cast<time_t>(uploadEveryMinutes) * 60;
    const time_t currentSlot = currentTime / intervalSeconds;

    if (observedUploadSlot == -1) {
        observedUploadSlot = currentSlot;
        struct tm localTime;
        if (localtime_r(&currentTime, &localTime) != nullptr
            && localTime.tm_min % uploadEveryMinutes == 0
            && consumptionBuffer.hasPending()) {
            scheduledUploadDue = true;
            logInfo("NTP-Synchronisation erfolgte innerhalb eines Upload-Zeitpunkts");
        }
        return;
    }
    if (currentSlot == observedUploadSlot) {
        return;
    }

    observedUploadSlot = currentSlot;
    scheduledUploadDue = consumptionBuffer.hasPending();
    if (scheduledUploadDue) {
        struct tm localTime;
        char formattedTime[24] = "unbekannte Zeit";
        if (localtime_r(&currentTime, &localTime) != nullptr) {
            strftime(formattedTime, sizeof(formattedTime), "%H:%M:%S %Z", &localTime);
        }
        logInfo(
            "Geplanter Upload-Zeitpunkt erreicht: "
            + String(formattedTime)
        );
    }
}

/**
 * @brief Uebertraegt einen ausstehenden Puffer und loescht ihn erst nach
 *        erfolgreicher Datenbankbestaetigung und anschliessendem Lesezugriff.
 * @return true, wenn Upload, Bestaetigung und Flash-Loeschung erfolgreich waren.
 */
bool uploadBufferedConsumption() {
    ConsumptionRecord batch[4];
    size_t batchCount = 0;
    uint64_t sequence = 0;
    if (!consumptionBuffer.getPendingBatch(batch, 4, batchCount, sequence)) {
        logError("Ausstehende Flash-Daten konnten nicht gelesen werden");
        return false;
    }

    const String deviceId = database.deviceId();
    if (deviceId.isEmpty()) {
        logError("ESP32-Geraete-ID konnte nicht erzeugt werden");
        return false;
    }

    if (!database.addConsumptionBatch(
            deviceId,
            sequence,
            batch,
            batchCount)) {
        logWarning("Datenbank-Upload fehlgeschlagen; Flash-Puffer bleibt erhalten");
        return false;
    }

    ConsumptionRecord confirmedRecords[4];
    size_t confirmedCount = 0;
    if (!database.readAll(confirmedRecords, 4, confirmedCount)
        || confirmedCount != 4) {
        logWarning(
            "Upload wurde verarbeitet, aber die Datenbankbestaetigung "
            "ist unvollstaendig; Flash-Puffer bleibt zur sicheren Wiederholung erhalten"
        );
        return false;
    }

    for (size_t index = 0; index < confirmedCount; ++index) {
        logInfo(
            "Bestaetigter Datenbankstand Zaehler "
            + String(confirmedRecords[index].counterId) + ": "
            + String(confirmedRecords[index].totalConsumption, 3)
        );
    }

    if (!consumptionBuffer.markBatchUploaded(sequence)) {
        logError(
            "Flash-Puffer konnte nach erfolgreichem Upload nicht geleert werden; "
            "derselbe Upload wird sicher wiederholt"
        );
        return false;
    }

    logInfo("Bestaetigter Flash-Puffer wurde geleert");
    return true;
}
}

/**
 * @brief Initialisiert serielle Ausgabe, Puffer und WLAN.
 */
void setup() {
    Serial.begin(115200);
    logInfo("System gestartet");

    if (!consumptionBuffer.begin()) {
        logError("Dauerhafter Verbrauchspuffer konnte nicht initialisiert werden");
    }

    if (wifi.connect()) {
        wifi.printStatus();
        configureNtp();
    } else {
        logError("WLAN-Verbindung fehlgeschlagen; automatische Wiederverbindung aktiv");
    }
}

/**
 * @brief Sammelt Impulse, verwaltet WLAN und uebertraegt faellige Datenpakete.
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

    if (wifi.isConnected()) {
        const unsigned long now = millis();
        configureNtp();
        logNtpStatus();
        monitorFlash(now);
        initializeDatabaseState(now);

        time_t synchronizedTime;
        if (getSynchronizedTime(synchronizedTime)) {
            updateUploadSchedule(synchronizedTime);
        } else {
            logDebug("Warte auf NTP-Zeitsynchronisation vor dem periodischen Upload");
        }

        const bool uploadDue =
            scheduledUploadDue
            || (flashStatsAvailable
                && flashThresholdReached
                && consumptionBuffer.hasPending());
        const bool retryAllowed =
            !uploadAttempted || now - lastUploadAttempt >= uploadRetryMs;

        if (databaseInitialized
            && consumptionBuffer.hasPending()
            && uploadDue
            && retryAllowed) {
            uploadAttempted = true;
            lastUploadAttempt = now;
            if (uploadBufferedConsumption()) {
                scheduledUploadDue = false;
            }
        }
    }

    delay(1000);
}
