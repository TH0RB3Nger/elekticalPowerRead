#include "StatusWebServer.h"

#include <WiFi.h>
#include <cstdio>

#include "ConsumptionBuffer.h"
#include "Logging.h"
#include "WirelessConnection.h"

namespace {
const char dashboardHtml[] PROGMEM = R"rawliteral(
<!doctype html><html lang="de"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Zaehlerstatus</title>
<style>
:root{color-scheme:light;font:16px "Trebuchet MS",sans-serif;background:#f1f4f2;color:#17231e}
body{max-width:760px;margin:32px auto;padding:0 16px}
h1{font-size:1.35rem;margin:0 0 20px;font-weight:650}
main{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:10px}
section{background:#fff;border:1px solid #dce4df;border-radius:6px;padding:16px}
h2{font-size:.78rem;text-transform:uppercase;color:#52645a;margin:0 0 12px}
p{font-size:1.35rem;font-variant-numeric:tabular-nums;margin:0}
small{display:block;color:#64746b;margin-top:6px}
.ok{color:#18774a}.bad{color:#b23b32}
</style>
<body><h1>Zaehlerstatus</h1><main>
<section><h2>W-LAN</h2><p id="wifi">Lade ...</p><small id="signal"></small></section>
<section><h2>Ausstehender Verbrauch</h2><p id="pending">Lade ...</p><small>nicht bestaetigt, in kWh</small></section>
<section><h2>RAM</h2><p id="ram">Lade ...</p><small id="ramDetail"></small></section>
<section><h2>CPU Hauptschleife</h2><p id="cpu">Lade ...</p><small>aktive Zeit, naeherungsweise</small></section>
</main><script>
const byId=id=>document.getElementById(id);
async function refresh(){try{const r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)throw Error();const s=await r.json();
byId('wifi').textContent=s.wifi?'Verbunden':'Getrennt';byId('wifi').className=s.wifi?'ok':'bad';
byId('signal').textContent=s.wifi?`${s.rssi} dBm`:'';
byId('pending').textContent=`${s.pendingKWh.toFixed(3)} kWh`;
const used=s.totalHeap-s.freeHeap;byId('ram').textContent=`${(used/1024).toFixed(1)} / ${(s.totalHeap/1024).toFixed(1)} KB`;
byId('ramDetail').textContent=`${Math.round(used*100/s.totalHeap)} % belegt, Minimum frei: ${(s.minFreeHeap/1024).toFixed(1)} KB`;
byId('cpu').textContent=`${s.loopLoad.toFixed(1)} %`;
}catch{byId('wifi').textContent='Status nicht erreichbar';byId('wifi').className='bad'}}
refresh();setInterval(refresh,5000);
</script></body></html>
)rawliteral";
}

StatusWebServer::StatusWebServer(
    WirelessConnection& wirelessConnection,
    const ConsumptionBuffer& consumptionBuffer
)
        : _server(80),
            _wifi(wirelessConnection),
            _consumptionBuffer(consumptionBuffer) {}

/// Bindet die statische Dashboard-Seite und die kompakte JSON-Statusroute.
void StatusWebServer::begin() {
    _server.on("/", HTTP_GET, [this]() {
        _server.send_P(200, "text/html; charset=utf-8", dashboardHtml);
    });
    _server.on("/api/status", HTTP_GET, [this]() {
        const bool connected = _wifi.isConnected();
        const uint32_t freeHeap = ESP.getFreeHeap();
        const uint32_t totalHeap = ESP.getHeapSize();
        char response[224];
        snprintf(
            response,
            sizeof(response),
            "{\"wifi\":%s,\"rssi\":%d,\"pendingKWh\":%.6g,"
            "\"freeHeap\":%u,\"totalHeap\":%u,\"minFreeHeap\":%u,"
            "\"loopLoad\":%.1f}",
            connected ? "true" : "false",
            connected ? WiFi.RSSI() : 0,
            _consumptionBuffer.getPendingConsumptionKWh(),
            static_cast<unsigned int>(freeHeap),
            static_cast<unsigned int>(totalHeap),
            static_cast<unsigned int>(ESP.getMinFreeHeap()),
            _loopLoadPercent
        );
        _server.send(200, "application/json", response);
    });
    _server.begin();
}

/// Laesst den synchronen Server aus dem bestehenden Hauptloop arbeiten.
void StatusWebServer::handleClient() {
    _server.handleClient();
}

/// Uebernimmt die letzte Loop-Lastmessung fuer die Statusroute.
void StatusWebServer::setLoopLoadPercent(double percent) {
    _loopLoadPercent = percent;
}

/// Gibt die lokale Dashboard-Adresse fuer den aktuellen WLAN-Link aus.
void StatusWebServer::logAddress() const {
    logInfo("Status-Weboberflaeche: http://" + WiFi.localIP().toString());
}