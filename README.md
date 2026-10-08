# Elektical Power Read

ESP32-basierter Stromverbrauchszaehler mit vier Impulseingaengen. Er erfasst
Impulse an GPIO 4 bis 7, puffert neue Verbrauchswerte dauerhaft im NVS und
uebertraegt sie gebuendelt an MariaDB/MySQL. Uploads erfolgen an festen
Viertelstundenzeitpunkten in der Zeitzone Europe/Berlin. Ein niedriger NVS-
Freispeicheranteil kann einen zusaetzlichen, sofortigen Upload ausloesen.

## Inhalt

- [Fuer Nutzer: Voraussetzungen und Einrichtung](#fuer-nutzer-voraussetzungen-und-einrichtung)
- [Fuer Entwickler: Projektstruktur](#fuer-entwickler-projektstruktur)
- [Ablauf und Ausfallsicherheit](#ablauf-und-ausfallsicherheit)
- [Konfiguration](#konfiguration)
- [Build, Upload und serielle Ausgabe](#build-upload-und-serielle-ausgabe)
- [Logging und Fehlerbehebung](#logging-und-fehlerbehebung)
- [Sicherheit und Betriebshinweise](#sicherheit-und-betriebshinweise)

## Fuer Nutzer: Voraussetzungen und Einrichtung

### Hardware und Dienste

- Arduino Nano ESP32 (ESP32-S3) und eine passende Stromversorgung.
- Vier Impulsgeber bzw. Zaehlerausgaenge an GPIO 4, 5, 6 und 7.
- WLAN mit Zugang zum MariaDB-/MySQL-Server und zu mindestens einem NTP-Server.
- MariaDB oder MySQL mit InnoDB-Unterstuetzung.
- PlatformIO in Visual Studio Code oder die PlatformIO Core CLI.

Die Eingangspins sind als `INPUT_PULLDOWN` konfiguriert und zaehlen steigende
Flanken. Stelle sicher, dass die Signalpegel elektrisch zum ESP32 passen
(3,3-V-Logik) und keine Spannung oberhalb der erlaubten GPIO-Grenzen anliegt.
Der Umrechnungsfaktor je Impuls muss zum angeschlossenen Zaehler passen.

### 1. Datenbank vorbereiten

Fuehre [database/schema.sql](database/schema.sql) mit einem administrativen
MariaDB-/MySQL-Konto auf dem Server aus. Das Skript erstellt die Datenbank
`energy_monitor`, die Verbrauchstabelle `energy_consumption`, den idempotenten
Synchronisationsstatus und die Startdatensaetze fuer GPIO 4 bis 7.

Erstelle danach einen dedizierten Anwendungsbenutzer. Er benoetigt Zugriff auf
`energy_consumption` zum Lesen, Einfuegen, Aktualisieren und Loeschen sowie auf
`energy_sync_state` zum Lesen, Einfuegen und Aktualisieren. Rechte sollten auf
die verwendeten Tabellen und den ESP32-Netzwerkstandort begrenzt werden. Die
genaue GRANT-Syntax haengt von der MariaDB-/MySQL-Version und Netzwerkkonfiguration
ab.

Weitere Informationen zu Tabellen und Datenbankfunktionen stehen in
[database/README.md](database/README.md).

### 2. Globale Konfiguration anlegen

Kopiere `include/Config.h.example` nach `include/Config.h`. Trage dort zentral
WLAN-Zugangsdaten, Datenbankverbindung sowie Laufzeit-, Intervall- und
Zeiteinstellungen in den passenden Bereichen ein:

- `Config::Wifi`: SSID und WLAN-Passwort.
- `Config::Database`: Host, Port, Datenbank, Benutzer und Passwort.
- `Config::Runtime`: Impulsfaktor, Entprellzeit, Uploadtakt,
  NVS-Schwelle, Wiederholungsintervalle, WLAN-Timeouts, Logging-Stufe und
  serielle Ausgabe.
- `Config::Time`: Zeitzone und NTP-Server.

Die echte `include/Config.h` wird von Git ignoriert, da sie Zugangsdaten
enthalten kann. Nur `Config.h.example` wird versioniert. Die Firmware bricht
beim Kompilieren mit einem Hinweis ab, wenn die lokale Konfiguration fehlt.
Nach Aenderungen an `Config.h` muss die Firmware neu gebaut und hochgeladen
werden; es handelt sich nicht um eine zur Laufzeit geladene Konfiguration.

### 3. Firmware bauen und hochladen

Oeffne das Projektverzeichnis in PlatformIO und waehle die Umgebung
`arduino_nano_esp32`. Die Core-Befehle aus dem Projektverzeichnis lauten:

```text
pio run
pio run --target upload
pio device monitor --baud 115200
```

Waehle bei mehreren angeschlossenen Boards den richtigen seriellen Port in
PlatformIO. Nach dem Start sollten serielle Meldungen fuer den NVS-Puffer,
die WLAN-Verbindung und die NTP-Synchronisation erscheinen. Ein Datenbankupload
ist erst moeglich, wenn das SQL-Schema vorbereitet und der ESP32 mit dem Server
verbunden ist.

### 4. Betrieb kontrollieren

Die serielle Ausgabe zeigt WLAN-Events, Datenbankverbindungsfehler, NTP-Status,
NVS-Kapazitaet und Upload-Ergebnisse entsprechend der eingestellten Logging-
Stufe. Standardmaessig wird der NVS-Speicher einmal pro Minute ueberprueft.
Ein erfolgreicher Upload bestaetigt den Datenbankstand und entfernt erst dann
den zugehoerigen Flash-Puffer.

## Fuer Entwickler: Projektstruktur

| Pfad | Aufgabe |
| --- | --- |
| `src/main.cpp` | Initialisiert Komponenten und orchestriert Impulserfassung, Netzwerk, NTP und Uploads. |
| `include/Config.h.example` | Vorlage fuer die lokale, zentrale Konfiguration. |
| `include/Config.h` | Lokale WLAN-, Datenbank- und Laufzeiteinstellungen; wird ignoriert. |
| `src/CountPin.cpp`, `include/CountPin.h` | Entprellte Interrupt-Impulszaehler fuer einen GPIO. |
| `src/Logging.cpp`, `include/Logging.h` | Zeitgestempelte Protokollierung mit konfigurierbaren Stufen. |
| `src/WirelessConnection.cpp`, `include/WirelessConnection.h` | WLAN-Verbindung, Reconnects, Events und Signalstaerke. |
| `src/NtpClock.cpp`, `include/NtpClock.h` | NTP-Start, Plausibilitaetspruefung und Europe/Berlin-Systemzeit. |
| `src/UploadScheduler.cpp`, `include/UploadScheduler.h` | Faelligkeitsverwaltung fuer feste Upload-Zeitpunkte. |
| `src/NvsStorageMonitor.cpp`, `include/NvsStorageMonitor.h` | Periodische NVS-Statistik und Schwellwertpruefung. |
| `src/PersistentConsumptionBuffer.cpp`, `include/PersistentConsumptionBuffer.h` | Dauerhafter, sequenzierter Verbrauchspuffer im NVS. |
| `src/ConsumptionSync.cpp`, `include/ConsumptionSync.h` | Datenbank-Initialisierung und bestaetigungsabhaengiger Upload/Loeschung. |
| `src/DatabaseConnection.cpp`, `include/DatabaseConnection.h` | MariaDB-/MySQL-Zugriff und transaktionale Batchverarbeitung. |
| `database/schema.sql` | Datenbankschema und Zaehlerdatensaetze fuer GPIO 4 bis 7. |

PlatformIO kompiliert die Quellen aus `src/` automatisch. Bibliotheksabhaengig-
keiten und die fuer ESP32 notwendige Ausschlussliste stehen in
`platformio.ini`. Die `lib_ignore`-Eintraege vermeiden Konflikte mit
WLAN-/Ethernet-Shield-Bibliotheken, die transitiv von der MySQL-Bibliothek
angeboten werden.

## Ablauf und Ausfallsicherheit

1. Jeder GPIO-Interrupt wird entprellt und erhoeht den zugehoerigen
   Impulszaehler.
2. `main.cpp` vergleicht die Zaehlerstaende mit den zuletzt gepufferten
   Staenden und schreibt nur neue Impulse als Verbrauchsdifferenz ins NVS.
3. `NtpClock` setzt die Systemzeit. Die Zeitzone
   `CET-1CEST,M3.5.0/2,M10.5.0/3` schaltet automatisch zwischen Normal- und
   Sommerzeit um.
4. `UploadScheduler` merkt einen faelligen Zeitpunkt vor, bis der Upload
   bestaetigt wurde. Der Datenbank-Synchronisierer liest zu Beginn die vier
   Zaehlerdatensaetze; unvollstaendige Datenbankeintraege verhindern den Upload.
5. Ein Batch wird mit ESP32-Geraete-ID und persistenter Sequenznummer
   transaktional angewendet. Der Server kann denselben Batch bei einem
   Verbindungsabbruch sicher wiedererkennen und doppelte Additionen vermeiden.
6. Nach dem Transaktionserfolg werden die Zaehlerstaende erneut gelesen. Erst
   nach dieser Bestaetigung wird der Flash-Puffer geleert und die Sequenz
   erhoeht. Bei Fehlern bleibt der Puffer bestehen und ein spaeterer Versuch
   kann den Batch erneut senden.
7. Die NVS-Ueberwachung laeuft unabhaengig von WLAN und kann bei Erreichen der
   konfigurierten Freispeicherschwelle einen zusaetzlichen Upload anfordern.
   Der Upload benoetigt weiterhin WLAN und eine initialisierte Datenbank.

Der NVS-Monitor misst freie Eintraege der NVS-Partition, nicht die gesamte
physische Flash-Kapazitaet und nicht den fuer Firmware reservierten Bereich.
Der Puffer ist ein fest dimensionierter Datensatz; die Zahl gespeicherter
Schluessel waechst nicht mit den Verbrauchswerten.

## Konfiguration

Die zentralen Werte liegen nach dem Kopieren in `include/Config.h`:

- `Config::Runtime::consumptionPerPulse`: Umrechnungsfaktor je Impuls; an den Zaehler anpassen.
- `Config::Runtime::debounceDelayMs`: Entprellzeit in Millisekunden.
- `Config::Runtime::uploadEveryMinutes`: Uploadabstand. Er muss ein positiver Teiler von 60
  sein. `15` ergibt lokale Zeitpunkte wie 15:00, 15:15, 15:30 und 15:45.
- `Config::Runtime::minimumFreeNvsPercent`: Schwelle fuer den zusaetzlichen Speicher-Upload.
- `Config::Runtime::flashMonitorIntervalMs`: Abstand zwischen NVS-Statistikabfragen.
- `Config::Runtime::uploadRetryMs` und `databaseReadRetryMs`: Wartezeiten fuer Wiederholungen.
- `Config::Runtime::minimumLogLevel`: Logging-Stufe (`LOG_ERROR`, `LOG_WARNING`,
  `LOG_INFO`, `LOG_DEBUG` oder `LOG_DEBUG_PLUS`).
- `Config::Runtime::wifiConnectionTimeoutMs`, `wifiReconnectIntervalMs` und
  `wifiSignalLogIntervalMs`: WLAN-Timeout und Statusintervalle.
- `Config::Runtime::serialBaudRate` und `mainLoopDelayMs`: serielle Baudrate
  und Schleifenpause.
- `Config::Time::timeZone`, `ntpServerPrimary`, `ntpServerSecondary`: lokale Zeitzone und NTP-Server.

Der Uploadplan berechnet feste Minutenmarken aus der synchronisierten Uhrzeit.
Ohne plausible NTP-Zeit gibt es keine zeitgesteuerten Uploads; ein Upload bei
kritischem NVS-Freispeicher bleibt dagegen moeglich, sobald WLAN und
Datenbankstartwerte verfuegbar sind.

## Build, Upload und serielle Ausgabe

Die wichtigsten PlatformIO-Aufgaben sind:

```text
pio run                         # Firmware kompilieren
pio run --target upload         # Firmware auf das Board laden
pio device monitor --baud 115200
pio run --target clean          # Build-Artefakte bereinigen
```

Das Board muss waehrend Upload und serieller Ausgabe am Rechner angeschlossen
sein. `monitor_speed` in `platformio.ini` und Baudrate des Monitors muessen
uebereinstimmen.

## Logging und Fehlerbehebung

Die Logging-Stufen und die zur Laufzeit gewaehlte Mindeststufe sind in
`include/Logging.h` beziehungsweise `src/Logging.cpp` definiert. Die
Ausgabestufen sind `ERROR`, `WARNING`, `INFO`, `DEBUG` und `DEBUG+`.
`DEBUG+` enthaelt zusaetzliche WLAN-Events und RSSI-Daten. Zeitstempel stammen
aus der Systemlaufzeit; sie stehen auch vor der NTP-Synchronisation zur
Verfuegung, sind dann aber keine Kalenderzeit.

| Symptom | Pruefen |
| --- | --- |
| WLAN verbindet nicht | `Config::Wifi`, SSID, Passwort, Empfang und serielle WLAN-Status-/Eventmeldungen kontrollieren. Der Reconnect wird automatisch wiederholt. |
| NTP-Zeit bleibt aus | Internetzugriff/DNS und NTP-Server in `Config::Time` pruefen. Ohne NTP sind zeitgesteuerte Uploads ausgesetzt. |
| Datenbankverbindung scheitert | `Config::Database`, Serveradresse, Port 3306, Netzwerkroute, Firewall und DB-Berechtigungen pruefen. |
| Startwerte fehlen | `database/schema.sql` ausfuehren; die Firmware erwartet Datensaetze fuer alle IDs 4 bis 7. |
| Upload wird wiederholt | Das ist bei Timeout oder verlorener Bestaetigung beabsichtigt. Geraete-ID und Sequenznummer verhindern doppelte Batchbuchung. Puffer nur nach Analyse manuell loeschen. |
| NVS-Puffer kann nicht geladen werden | Meldung nicht ignorieren: ein ungueltiger Puffer wird nicht automatisch ueberschrieben, um moeglicherweise ungesendete Werte nicht still zu verwerfen. |
| Impulse fehlen | Signalpegel, steigende Flanke, GPIO-Zuordnung, Entprellzeit und Impulsfaktor kontrollieren. |

## Sicherheit und Betriebshinweise

- Die MariaDB-/MySQL-Verbindung des aktuellen Firmwaremoduls verwendet kein
  TLS. Verwende sie nur in einem kontrollierten, vertrauenswuerdigen LAN und
  exponiere den Datenbankport nicht im oeffentlichen Internet.
- Zugangsdaten gehoeren in die ignorierten lokalen Konfigurationsdateien und
  nicht in Commits, Screenshots oder Logs.
- Der Puffer wird bei jedem neu erfassten Verbrauchsschritt dauerhaft
  aktualisiert. Das erhoeht Robustheit gegen Neustarts, beansprucht aber
  Flash-Schreibzyklen. Bei sehr hoher Impulsrate muss die Persistenzstrategie
  gegen Datenverlust-Risiko und Flash-Lebensdauer abgewogen werden.
- Bei Stromausfall waehrend eines einzelnen NVS-Schreibvorgangs haengt die
  Wiederherstellbarkeit von der NVS-/Flash-Implementierung ab. Die
  Batch-Sequenzierung schuetzt vor doppelter DB-Verbuchung, ersetzt aber keine
  externe unterbrechungsfreie Stromversorgung.
- NTP benoetigt Netzwerkzugriff. Die Systemuhr wird nicht als batteriegepufferte
  Echtzeituhr behandelt.
