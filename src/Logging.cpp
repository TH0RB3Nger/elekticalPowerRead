#include "Logging.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>

#include "Config.h"

namespace {
/// Die Mindeststufe wird zentral in Config.h festgelegt.
constexpr LogLevel currentLogLevel = Config::Runtime::minimumLogLevel;

/// Unterhalb dieses Unix-Zeitpunkts gilt die Uhr als noch nicht synchronisiert.
constexpr time_t plausibleEpoch = 1700000000;
SemaphoreHandle_t logMutex = nullptr;
}

/// Erstellt vor Taskstart den Mutex fuer zusammenhaengende Logzeilen.
bool beginLogging() {
    if (logMutex != nullptr) {
        return true;
    }
    logMutex = xSemaphoreCreateMutex();
    return logMutex != nullptr;
}

/**
 * @brief Formatiert lokale Zeit und Stufe und gibt die Meldung aus.
 */
void logMessage(LogLevel level, const String& message) {
    if (level > currentLogLevel) {
        return;
    }

    const bool locked = logMutex != nullptr
        && xSemaphoreTake(logMutex, portMAX_DELAY) == pdTRUE;

    char timestamp[40];
    const time_t currentTime = time(nullptr);
    struct tm localTime;
    const bool hasLocalTime = currentTime >= plausibleEpoch
        && localtime_r(&currentTime, &localTime) != nullptr
        && strftime(
            timestamp,
            sizeof(timestamp),
            "[%d.%m.%Y %H:%M:%S %Z]",
            &localTime) > 0;
    if (!hasLocalTime) {
        snprintf(timestamp, sizeof(timestamp), "[Uhrzeit nicht synchronisiert]");
    }

    const char* levelName = "INFO";
    switch (level) {
        case LOG_ERROR:
            levelName = "ERROR";
            break;
        case LOG_WARNING:
            levelName = "WARNING";
            break;
        case LOG_INFO:
            levelName = "INFO";
            break;
        case LOG_DEBUG:
            levelName = "DEBUG";
            break;
        case LOG_DEBUG_PLUS:
            levelName = "DEBUG+";
            break;
        default:
            levelName = "INFO";
            break;
    }

    Serial.print(timestamp);
    Serial.print("[");
    Serial.print(levelName);
    Serial.print("] ");
    Serial.println(message);

    if (locked) {
        xSemaphoreGive(logMutex);
    }
}

/// Leitet eine Fehlermeldung an den gemeinsamen Logger weiter.
void logError(const String& message) {
    logMessage(LOG_ERROR, message);
}

/// Leitet eine Warnmeldung an den gemeinsamen Logger weiter.
void logWarning(const String& message) {
    logMessage(LOG_WARNING, message);
}

/// Leitet eine Infomeldung an den gemeinsamen Logger weiter.
void logInfo(const String& message) {
    logMessage(LOG_INFO, message);
}

/// Leitet eine Debugmeldung an den gemeinsamen Logger weiter.
void logDebug(const String& message) {
    logMessage(LOG_DEBUG, message);
}

/// Leitet eine besonders ausfuehrliche Debugmeldung an den Logger weiter.
void logDebugPlus(const String& message) {
    logMessage(LOG_DEBUG_PLUS, message);
}
