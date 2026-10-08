#include "UploadScheduler.h"

#include "Logging.h"

UploadScheduler::UploadScheduler(uint8_t intervalMinutes)
    : _intervalMinutes(intervalMinutes) {
    if (_intervalMinutes == 0 || 60 % _intervalMinutes != 0) {
        logError("Uploadintervall muss ein positiver Teiler von 60 sein");
        _intervalMinutes = 15;
    }
}

bool UploadScheduler::update(time_t currentTime, bool hasPendingData) {
    const time_t intervalSeconds =
        static_cast<time_t>(_intervalMinutes) * 60;
    const time_t currentSlot = currentTime / intervalSeconds;

    if (_observedSlot == -1) {
        _observedSlot = currentSlot;

        struct tm localTime;
        if (hasPendingData
            && localtime_r(&currentTime, &localTime) != nullptr
            && localTime.tm_min % _intervalMinutes == 0) {
            _due = true;
            logInfo("NTP-Synchronisation erfolgte waehrend eines Upload-Zeitpunkts");
        }
        return _due;
    }

    if (currentSlot == _observedSlot) {
        return _due;
    }

    _observedSlot = currentSlot;
    if (!hasPendingData) {
        return _due;
    }

    _due = true;
    struct tm localTime;
    char formattedTime[24] = "unbekannte Zeit";
    if (localtime_r(&currentTime, &localTime) != nullptr) {
        strftime(formattedTime, sizeof(formattedTime), "%H:%M:%S %Z", &localTime);
    }
    logInfo("Geplanter Upload-Zeitpunkt erreicht: " + String(formattedTime));
    return _due;
}

bool UploadScheduler::isDue() const {
    return _due;
}

void UploadScheduler::markCompleted() {
    _due = false;
}
