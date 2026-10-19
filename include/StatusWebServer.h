#pragma once

#include <Arduino.h>
#include <WebServer.h>

class ConsumptionBuffer;
class WirelessConnection;

/**
 * @brief Stellt die lokale Statusseite und deren JSON-Endpunkt bereit.
 */
class StatusWebServer {
public:
    StatusWebServer(
        WirelessConnection& wirelessConnection,
        const ConsumptionBuffer& consumptionBuffer
    );

    /// Registriert HTTP-Routen und startet den lokalen Webserver.
    void begin();

    /// Bearbeitet eingehende HTTP-Anfragen im Hauptloop.
    void handleClient();

    /// Aktualisiert die naeherungsweise gemessene Hauptloop-Last.
    void setLoopLoadPercent(double percent);

    /// Protokolliert die lokale URL bei bestehender WLAN-Verbindung.
    void logAddress() const;

private:
    WebServer _server;
    WirelessConnection& _wifi;
    const ConsumptionBuffer& _consumptionBuffer;
    double _loopLoadPercent = 0.0;
};