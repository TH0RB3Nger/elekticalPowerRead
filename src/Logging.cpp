#include "Logging.h"

#include <time.h>

#include "Config.h"

namespace {
/// Startwert aus Config.h; serielle Befehle duerfen ihn zur Laufzeit aendern.
LogLevel currentLogLevel = Config::Runtime::minimumLogLevel;

/// Unterhalb dieses Unix-Zeitpunkts gilt die Uhr als noch nicht synchronisiert.
constexpr time_t plausibleEpoch = 1700000000;
}

/// Setzt die Laufzeitstufe des zentralen Loggers.
void setLogLevel(LogLevel level) {
    currentLogLevel = level;
}

/// Gibt die aktuell aktive Laufzeitstufe zurueck.
LogLevel getLogLevel() {
    return currentLogLevel;
}

/// Wandelt einen Log-Level in seine kurze serielle Bezeichnung um.
const char* getLogLevelName(LogLevel level) {
    switch (level) {
        case LOG_ERROR: return "ERROR";
        case LOG_WARNING: return "WARNING";
        case LOG_INFO: return "INFO";
        case LOG_DEBUG: return "DEBUG";
        case LOG_DEBUG_PLUS: return "DEBUG_PLUS";
        default: return "UNKNOWN";
    }
}

/**
 * @brief Formatiert lokale Zeit und Stufe und gibt die Meldung aus.
 */
void logMessage(LogLevel level, const String& message) {
    if (level > currentLogLevel) {
        return;
    }

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

    const char* levelName = getLogLevelName(level);
    Serial.print(timestamp);
    Serial.print("[");
    Serial.print(levelName);
    Serial.print("] ");
    Serial.println(message);
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
