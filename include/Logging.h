#pragma once

#include <Arduino.h>

enum LogLevel {
    LOG_ERROR = 1,
    LOG_WARNING = 2,
    LOG_INFO = 3,
    LOG_DEBUG = 4,
    LOG_DEBUG_PLUS = 5
};

/// Gibt eine Meldung aus, wenn ihre Stufe die konfigurierte Stufe nicht uebersteigt.
void logMessage(LogLevel level, const String& message);

/// Protokolliert einen Fehler.
void logError(const String& message);

/// Protokolliert eine Warnung.
void logWarning(const String& message);

/// Protokolliert eine normale Statusmeldung.
void logInfo(const String& message);

/// Protokolliert eine Debugmeldung.
void logDebug(const String& message);

/// Protokolliert eine besonders ausfuehrliche Debugmeldung.
void logDebugPlus(const String& message);
