# Tests und Verifikation

In diesem Verzeichnis sind derzeit keine PlatformIO-Tests definiert.

## Tests ergaenzen

Fuege hier fokussierte Tests fuer hardwareunabhaengige Logik hinzu, zum
Beispiel:

- Upload-Faelligkeit und Intervallgrenzen in `UploadScheduler`.
- Batch-Sequenzen und Verhalten bei Wiederholungen.
- Validierung und Grenzfaelle der Verbrauchspufferrechnung.
- Pulsdauer- und Mindestabstandsberechnungen mit isolierten Eingabewerten.

Tests sollen Seiteneffekte von GPIO, WLAN, NVS und Datenbankzugriffen
abstrahieren, damit sie lokal und reproduzierbar laufen. Hardwareintegration
und reale S0-Signalpegel muessen zusaetzlich am Zielboard geprueft werden.

## Aktuelle Verifikation

Im Projektverzeichnis baut folgender Befehl die Firmware fuer den Arduino
Nano ESP32:

```text
pio run
```

Ein erfolgreicher Build bestaetigt die Kompilierung, ersetzt aber keine
Laufzeit- oder Hardwaretests. Fuer den Anwenderablauf siehe
[Firmware installieren](../README.md#firmware-installieren); fuer Build- und
Architekturregeln siehe das
[Entwicklerhandbuch](../README_DEVELOPERS.md#entwicklungsumgebung-und-build).
