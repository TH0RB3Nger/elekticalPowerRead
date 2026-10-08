#pragma once

#include <Arduino.h>
#include <WiFi.h>

class WirelessConnection {
private:
    /// Netzwerkkennung, Zugangsschluessel und Verbindungstimeout.
    const char* _ssid;
    const char* _password;
    const unsigned long _maxConnectionTime;

    /// Zeitpunkte fuer Reconnect und RSSI-Protokollierung.
    unsigned long _lastReconnectAttempt;
    unsigned long _lastSignalLog;

    /// Verhindert doppelte Registrierung des globalen WLAN-Eventhandlers.
    bool _eventHandlerRegistered;

public:
    /**
     * @brief Erstellt einen WLAN-Client mit den angegebenen Zugangsdaten.
     */
    WirelessConnection(
        const char* ssid,
        const char* password,
        unsigned long maxConnectionTime = 10000
    );

    /// Startet die Verbindung und wartet bis zum Timeout auf Erfolg.
    bool connect();

    /// Prueft den aktuellen WLAN-Verbindungsstatus.
    bool isConnected() const;

    /// Startet bei Verbindungsverlust zeitlich begrenzte Reconnectversuche.
    void maintainConnection();

    /// Gibt Verbindungsstatus und periodische Signalstaerke aus.
    void printStatus();

    /// Verarbeitet im Eventtask empfangene WLAN-Ereignisse im Haupttask.
    void processEvents();
};
