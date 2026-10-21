# Schnittstellen und Konfiguration

Dieses Verzeichnis enthaelt die oeffentlichen Header der Firmwaremodule und
die zentrale Konfigurationsvorlage.

## Konfiguration

Kopiere `Config.h.example` nach `Config.h` und trage dort WLAN-, Datenbank-,
Hardware- und Laufzeitwerte ein. `Config.h.example` ist versioniert;
`Config.h` enthaelt lokale Zugangsdaten und wird von Git ignoriert.
Konfigurationsdetails fuer Anwender stehen in der [Anwender-README](../README.md#konfiguration).

## Modulheader

Jedes Modul stellt seine oeffentliche Klasse und die dazugehoerigen
Deklarationen in einem Header bereit. Die Implementierung liegt in der
gleichnamigen `.cpp`-Datei unter `src/`. Wichtige Schnittstellen sind:

- `CountPin.h`: GPIO-Zaehler und ISR-Diagnosezugriff.
- `PulseAccounting.h`: Uebertragung neuer Impulsdifferenzen in den Puffer.
- `ConsumptionBuffer.h` und `ConsumptionBufferState.h`: synchronisierter
  Verbrauchszustand und Datenformat.
- `ConsumptionSync.h`: Upload-Snapshot und bestaetigungsabhaengige Verrechnung.
- `DatabaseConnection.h` und `DatabaseWorker.h`: Datenbankzugriff und
  serialisierte Worker-Auftraege.
- `SerialCommandProcessor.h`, `StatusLed.h` und `StatusWebServer.h`:
  Bedienungs- und Statusschnittstellen.
- `WirelessConnection.h`, `NtpClock.h` und `UploadScheduler.h`:
  Netzwerk und Zeitsteuerung.
- `NvsConsumptionStore.h`, `NvsStorageMonitor.h` und `RamStorageMonitor.h`:
  Persistenz und Speicherueberwachung.

Neue oeffentliche Funktionen gehoeren in den passenden Header. Private
Implementierungsdetails bleiben in der `.cpp`-Datei. Die Architektur- und
Synchronisationsregeln stehen im
[Entwicklerhandbuch](../README_DEVELOPERS.md#modulstruktur-und-verantwortlichkeiten).
