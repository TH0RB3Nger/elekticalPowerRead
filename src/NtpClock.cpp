#include "NtpClock.h"

#include <time.h>

#include "Config.h"
#include "Logging.h"

namespace {
/// Unterhalb dieses Unix-Zeitpunkts gilt die Uhr als noch nicht synchronisiert.
constexpr time_t plausibleEpoch = 1700000000;
}

void NtpClock::begin() {
    if (_started) {
        return;
    }

    logDebug(
        "Richte SNTP ein; Server: "
        + String(Config::Time::ntpServerPrimary) + ", "
        + String(Config::Time::ntpServerSecondary)
    );
    configTzTime(
        Config::Time::timeZone,
        Config::Time::ntpServerPrimary,
        Config::Time::ntpServerSecondary
    );
    _started = true;
    logInfo("NTP gestartet; Europe/Berlin mit automatischer Sommerzeit aktiviert");
}

bool NtpClock::getTime(time_t& currentTime) const {
    currentTime = time(nullptr);
    const bool plausible = _started && currentTime >= plausibleEpoch;
    return plausible;
}

void NtpClock::logStatus() {
    if (_synchronizationLogged) {
        return;
    }

    time_t currentTime;
    if (!getTime(currentTime)) {
        const unsigned long now = millis();
        if (_lastWaitingLog == 0 || now - _lastWaitingLog >= 30000UL) {
            _lastWaitingLog = now;
            logDebug("NTP-Zeit noch nicht synchronisiert; warte auf Zeitserver");
        }
        return;
    }

    struct tm localTime;
    if (localtime_r(&currentTime, &localTime) == nullptr) {
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

    _synchronizationLogged = true;
    logInfo("NTP-Zeit synchronisiert: " + String(formattedTime));
}
