#include "WirelessConnection.h"

#include "Logging.h"

namespace {
/// Bitmasken fuer WLAN-Ereignisse, die im Hauptprogramm protokolliert werden.
enum WiFiEventFlag : uint32_t {
    PENDING_STA_STARTED = 1UL << 0,
    PENDING_STA_CONNECTED = 1UL << 1,
    PENDING_STA_DISCONNECTED = 1UL << 2,
    PENDING_STA_GOT_IP = 1UL << 3,
    PENDING_STA_LOST_IP = 1UL << 4,
    PENDING_STA_STOPPED = 1UL << 5
};

/// Synchronisation zwischen ESP32-WLAN-Eventtask und loop()-Task.
portMUX_TYPE wifiEventMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t pendingWiFiEvents = 0;
volatile uint32_t assignedIpAddress = 0;
volatile uint8_t disconnectReason = 0;

/**
 * @brief Kopiert kleine Ereignisdaten threadsicher in die ausstehende Bitmaske.
 */
void handleWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    uint32_t eventFlag = 0;

    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_START:
            eventFlag = PENDING_STA_STARTED;
            break;
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            eventFlag = PENDING_STA_CONNECTED;
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            portENTER_CRITICAL(&wifiEventMux);
            disconnectReason = info.wifi_sta_disconnected.reason;
            pendingWiFiEvents |= PENDING_STA_DISCONNECTED;
            portEXIT_CRITICAL(&wifiEventMux);
            return;
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            portENTER_CRITICAL(&wifiEventMux);
            assignedIpAddress = info.got_ip.ip_info.ip.addr;
            pendingWiFiEvents |= PENDING_STA_GOT_IP;
            portEXIT_CRITICAL(&wifiEventMux);
            return;
        case ARDUINO_EVENT_WIFI_STA_LOST_IP:
            eventFlag = PENDING_STA_LOST_IP;
            break;
        case ARDUINO_EVENT_WIFI_STA_STOP:
            eventFlag = PENDING_STA_STOPPED;
            break;
        default:
            return;
    }

    portENTER_CRITICAL(&wifiEventMux);
    pendingWiFiEvents |= eventFlag;
    portEXIT_CRITICAL(&wifiEventMux);
}

/**
 * @brief Wandelt einen Arduino-WLAN-Status in einen lesbaren Namen um.
 */
const char* getWiFiStatusName(wl_status_t status) {
    switch (status) {
        case WL_IDLE_STATUS:
            return "WL_IDLE_STATUS";
        case WL_NO_SSID_AVAIL:
            return "WL_NO_SSID_AVAIL";
        case WL_SCAN_COMPLETED:
            return "WL_SCAN_COMPLETED";
        case WL_CONNECTED:
            return "WL_CONNECTED";
        case WL_CONNECT_FAILED:
            return "WL_CONNECT_FAILED";
        case WL_CONNECTION_LOST:
            return "WL_CONNECTION_LOST";
        case WL_DISCONNECTED:
            return "WL_DISCONNECTED";
        default:
            return "UNKNOWN";
    }
}
}

/**
 * @brief Speichert WLAN-Zugangsdaten und initialisiert den Verbindungszustand.
 */
WirelessConnection::WirelessConnection(
    const char* ssid,
    const char* password,
    unsigned long maxConnectionTime,
    unsigned long reconnectInterval,
    unsigned long signalLogInterval
)
    : _ssid(ssid),
      _password(password),
      _maxConnectionTime(maxConnectionTime),
      _reconnectInterval(reconnectInterval),
      _signalLogInterval(signalLogInterval),
      _lastReconnectAttempt(0),
      _lastSignalLog(0),
      _eventHandlerRegistered(false) {}

/**
 * @brief Registriert Events, startet Stationmodus und wartet auf Verbindung.
 */
bool WirelessConnection::connect() {
    if (!_eventHandlerRegistered) {
        WiFi.onEvent(handleWiFiEvent);
        _eventHandlerRegistered = true;
    }

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);
    WiFi.begin(_ssid, _password);

    const unsigned long startTime = millis();
    logInfo("Verbinde mit WLAN: " + String(_ssid));

    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - startTime >= _maxConnectionTime) {
            logError(
                "WLAN-Verbindung fehlgeschlagen. Status: "
                + String(getWiFiStatusName(WiFi.status()))
            );
            return false;
        }

        delay(100);
    }

    logInfo("WLAN verbunden. IP: " + WiFi.localIP().toString());
    return true;
}

/// Meldet, ob der ESP32 derzeit mit einem Zugangspunkt verbunden ist.
bool WirelessConnection::isConnected() const {
    return WiFi.status() == WL_CONNECTED;
}

/**
 * @brief Versucht nach Verbindungsverlust im Abstand von fuenf Sekunden erneut
 *        eine Verbindung aufzubauen.
 */
void WirelessConnection::maintainConnection() {
    const wl_status_t status = WiFi.status();
    if (status == WL_CONNECTED) {
        return;
    }

    const unsigned long now = millis();
    if (now - _lastReconnectAttempt < _reconnectInterval) {
        return;
    }

    _lastReconnectAttempt = now;
    logWarning(
        "WLAN getrennt. Neuer Verbindungsversuch. Status: "
        + String(getWiFiStatusName(status))
    );

    WiFi.disconnect(false);
    delay(250);
    WiFi.begin(_ssid, _password);
}

/**
 * @brief Protokolliert den Status und im Debug+-Modus periodisch den RSSI.
 */
void WirelessConnection::printStatus() {
    logDebug("WiFi-Status: " + String(getWiFiStatusName(WiFi.status())));

    const unsigned long now = millis();
    if (WiFi.status() == WL_CONNECTED
        && now - _lastSignalLog >= _signalLogInterval) {
        _lastSignalLog = now;
        logDebugPlus("WLAN-Signalstaerke: " + String(WiFi.RSSI()) + " dBm");
    }
}

/**
 * @brief Holt ausstehende Ereignisse threadsicher ab und protokolliert sie.
 */
void WirelessConnection::processEvents() {
    uint32_t events;
    uint32_t ipAddress;
    uint8_t reason;

    portENTER_CRITICAL(&wifiEventMux);
    events = pendingWiFiEvents;
    pendingWiFiEvents = 0;
    ipAddress = assignedIpAddress;
    reason = disconnectReason;
    portEXIT_CRITICAL(&wifiEventMux);

    if (events & PENDING_STA_STARTED) {
        logDebugPlus("WLAN-Station gestartet");
    }
    if (events & PENDING_STA_CONNECTED) {
        logDebugPlus("Mit WLAN-Zugangspunkt verbunden");
    }
    if (events & PENDING_STA_GOT_IP) {
        logDebugPlus("IP-Adresse erhalten: " + IPAddress(ipAddress).toString());
    }
    if (events & PENDING_STA_DISCONNECTED) {
        logDebugPlus("WLAN-Verbindung getrennt; Grundcode: " + String(reason));
    }
    if (events & PENDING_STA_LOST_IP) {
        logDebugPlus("IP-Adresse verloren");
    }
    if (events & PENDING_STA_STOPPED) {
        logDebugPlus("WLAN-Station gestoppt");
    }
}
