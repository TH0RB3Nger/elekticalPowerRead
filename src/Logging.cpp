#include "Logging.h"

namespace {
/// Aktuelle Filterstufe; fuer weniger Ausgabe auf LOG_INFO oder LOG_DEBUG setzen.
constexpr LogLevel currentLogLevel = LOG_DEBUG_PLUS;
}

/**
 * @brief Formatiert Zeit seit Systemstart und Stufe und gibt die Meldung aus.
 */
void logMessage(LogLevel level, const String& message) {
    if (level > currentLogLevel) {
        return;
    }

    const unsigned long uptime = millis();
    char timestamp[20];
    snprintf(
        timestamp,
        sizeof(timestamp),
        "[%lu.%03lus]",
        uptime / 1000UL,
        uptime % 1000UL
    );

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
