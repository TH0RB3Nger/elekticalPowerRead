# Elektical Power Read – Entwicklerhandbuch

ESP32-basierter Stromverbrauchszaehler mit vier konfigurierbaren
Impulseingaengen. Neue Verbrauchswerte werden standardmaessig im RAM gepuffert
(optional dauerhaft im NVS) und gebuendelt an MariaDB/MySQL uebertragen. Uploads
erfolgen an festen Viertelstundenzeitpunkten in der Zeitzone Europe/Berlin. Im
NVS-Modus kann wenig NVS-Freispeicher einen zusaetzlichen Upload ausloesen. Im
RAM-Modus wird stattdessen die freie Heap-Reserve ueberwacht.

Dieses Dokument beschreibt interne Architektur, Datenfluss, Synchronisation,
Fehlerbehandlung und Entwicklungsablaeufe. Fuer Installation, Verdrahtung und
Bedienung des Geraets siehe die [Anwender-README](README.md).

## KI-Hinweis und Lizenz

Dieses Projekt wurde mit Unterstuetzung kuenstlicher Intelligenz (KI) erstellt
und weiterentwickelt. KI-generierter oder KI-unterstuetzter Quellcode,
Kommentare und Dokumentation koennen Fehler enthalten. Pruefe Aenderungen
fachlich und teste sie vor einer Freigabe oder einem Einsatz auf Hardware.
Die Software wird ohne Gewaehr bereitgestellt.

Der Projektcode steht unter der **GNU General Public License v3.0 only
(GPL-3.0-only)**. Einzelheiten und Lizenzbedingungen stehen in der
[LICENSE](LICENSE)-Datei.

## Inhalt

- [Ueberblick fuer Entwickler](#ueberblick-fuer-entwickler)
- [Systemarchitektur](#systemarchitektur)
- [Datenfluss und Threading](#datenfluss-und-threading)
- [Synchronisationsregeln](#synchronisationsregeln)
- [Modulstruktur und Verantwortlichkeiten](#modulstruktur-und-verantwortlichkeiten)
- [Konfigurations- und Persistenzvertrag](#konfigurations--und-persistenzvertrag)
- [Entwicklungsumgebung und Build](#entwicklungsumgebung-und-build)
- [Projektstruktur](#projektstruktur)
- [RGB-Statusanzeige](#rgb-statusanzeige)
- [Ablauf und Ausfallsicherheit](#ablauf-und-ausfallsicherheit)
- [Ueberlauf- und Wertebereichsschutz](#ueberlauf--und-wertebereichsschutz)
- [Konfiguration](#konfiguration)
- [Build, Upload und serielle Ausgabe](#build-upload-und-serielle-ausgabe)
- [Logging und Fehlerbehebung](#logging-und-fehlerbehebung)
- [Sicherheit und Betriebshinweise](#sicherheit-und-betriebshinweise)

## Ueberblick fuer Entwickler

Die Firmware ist fuer Arduino Nano ESP32 mit PlatformIO, Arduino-Core und
FreeRTOS gebaut. Der `loop()`-Task ist der zentrale Koordinator: er liest
Seriellbefehle, bedient WLAN/NTP und Webserver, aktualisiert Zaehlerdifferenzen
und entscheidet, wann Datenbankauftraege faellig sind. Blockierende SQL-
Operationen werden einem separaten Worker-Task uebergeben.

Die Grundregeln der Architektur sind:

1. GPIO-Interrupts zaehlen nur Flanken und pflegen kleine, interruptgeschuetzte
   Zustandswerte. Sie allozieren keinen dynamischen Speicher und loggen nicht.
2. Verbrauchsberechnung und Statusmeldungen laufen ausserhalb der ISR im
   `PulseAccounting`-Modul.
3. `DatabaseWorker` ist der einzige asynchrone Besitzer der DB-Auftragsqueue.
   Eine einzelne Aufgabe darf jeweils in Bearbeitung sein.
4. `ConsumptionBuffer` serialisiert alle Leser und Schreiber mit einem Mutex.
   Ein Upload bestaetigt nur seinen gesendeten Snapshot.
5. Im RAM-Modus sind Verbrauchswerte fluechtig; Sequenznummer und
   Ueberlauf-Checkpoint bleiben persistent. Im NVS-Modus wird der
   Verbrauchszustand nach jeder Aenderung gesichert.

## Systemarchitektur

```text
S0-Eingang
  -> CountPin ISR
  -> PulseAccounting (loop-Task)
  -> ConsumptionBuffer (Mutex; RAM/NVS)
  -> ConsumptionSync (fester Batch + Sequenz)
  -> DatabaseWorker queue
  -> MySQL/MariaDB transaction
  -> Ergebnisqueue
  -> loop-Task aktualisiert Status und Scheduler
```

Weitere Hintergrundaufgaben sind der serielle Kommandoempfang und die
Status-LED. Der Webserver wird dagegen synchron im Hauptloop bedient.
`SerialCommandProcessor` pausiert GPIO-Interrupts sofort fuer `STOP`/`KILL`,
uebergibt den Befehl anschliessend ueber eine Queue und laesst den Hauptloop
den Programmzustand aendern.

## Datenfluss und Threading

### Impulsaufnahme und Buffer

`CountPin` registriert beide Signalflanken. Die ISR validiert Polaritaet,
Pulsdauer und Mindestabstand und erhoeht den 64-Bit-Zaehler saturierend.
`PulseAccounting` haelt je Kanal den zuletzt uebernommenen Stand. Im Hauptloop
berechnet es nur die Differenz und schreibt sie in `ConsumptionBuffer`.
`STOP` pausiert die Interrupts und ruft danach `flush()` auf, damit auch der
letzte Counterstand in den Puffer gelangt.

### Upload und Wiederholung

`ConsumptionSync` kopiert einen konsistenten Batch aus dem Buffer. Der Batch
bleibt bis zur vollstaendigen DB-Bestaetigung unveraendert gespeichert, auch
wenn ein Versuch scheitert. Der Worker fuehrt die Datenbanktransaktion aus:
Sequenzstatus sperren, vier Kanalzeilen mit einem UTC-Zeitstempel schreiben,
Sequenz fortschreiben und committen. Erst nach Commit werden die Batchwerte
atomar aus dem lokalen Puffer abgezogen.

Messwerte, die waehrend des DB-Aufrufs neu erfasst werden, landen weiterhin im
Buffer. Die Bestaetigung zieht nur die Werte des bereits gesendeten Snapshots
ab. Schlaegt das NVS-Checkpoint nach einem erfolgreichen DB-Commit fehl, wird
derselbe Batch erneut gesendet; die persistente DB-Sequenz bestaetigt diesen
idempotent und erlaubt danach die lokale Bestaetigung.

### Datenbank-Worker

`DatabaseWorker` besitzt Befehls- und Ergebnisqueue sowie den Worker-Task.
Uploads und Verbindungspruefungen verwenden denselben seriellen Pfad, damit
`WiFiClient` und `MySQL_Connection` nie parallel aus Haupt- und Worker-Task
bedient werden. Der Haupttask setzt den Worker durch `begin()` einmalig auf
Core 0 auf, stellt mit `requestUpload()` oder `requestConnectionCheck()` einen
Auftrag ein und verarbeitet Ergebnisse mit `pollResult()`.

Falls Logger-Mutex oder Datenbank-Task nicht gestartet werden koennen, nutzt
der Hauptloop einen synchronen Fallback. In diesem Modus gibt es keine
gleichzeitigen Datenbankoperationen, da alle direkten Aufrufe aus demselben
Haupttask kommen.

### STOP, KILL und Tasks

- `STOP`: serielle Eingabe pausiert GPIO-Interrupts sofort; der Hauptloop
  uebernimmt den letzten Zaehlerstand, wechselt in `Stopping` und versucht den
  gesamten Puffer hochzuladen. Das Geraet meldet den abgeschlossenen Zustand
  erst bei leerem Puffer.
- `START`: hebt `STOP`/`KILL` auf und nimmt die Impulserfassung wieder auf.
- `KILL`: pausiert Impulse ohne Flush. Ungesicherte Zaehlerdifferenzen und
  RAM-Pufferwerte koennen verloren gehen.
- `StatusLed`: liest Statuswerte unter einer kurzen ESP32-Critical-Section und
  rendert LED-Zustaende in einem eigenen Task.
- `Logging`: serialisiert Ausgabe und Log-Level mit einem FreeRTOS-Mutex.

Die Hauptschleife und Arduino-Framework-APIs bleiben im Arduino-Core.
Datenbankauftraege laufen auf Core 0. Webserver- und WLAN-Arbeit wird nicht in
den Datenbank-Worker verschoben.

## Synchronisationsregeln

- Keine serielle Ausgabe, dynamische String-Erzeugung oder NVS-/Netzwerkzugriffe
  aus `CountPin::handleInterrupt`.
- Ein Datenbankkommando wird erst nach Abholung seines Ergebnisses als beendet
  betrachtet. Dadurch kann kein zweiter Befehl die gemeinsame Verbindung
  gleichzeitig benutzen.
- Der Puffer-Mutex umfasst Snapshots, Updates, Summenabfragen und
  Bestaetigungen. NVS-Schreibvorgaenge erfolgen unter diesem Mutex, damit keine
  konkurrierende Zustandskopie verloren geht.
- Die DB-Transaktion wird nicht unter dem Puffer-Mutex ausgefuehrt. Der Worker
  haelt nur seinen Batch-Snapshot; Impulse und HTTP-Statusabfragen bleiben
  waehrend des Netzwerkzugriffs moeglich.
- `SerialCommandProcessor`-Callback pausiert nur die GPIO-Interrupts und setzt
  LED-Anzeigen. Der eigentliche Programmzustand wird im Hauptloop bearbeitet.
- Shared Logger-Status ist mutex-geschuetzt. `volatile` wird nicht als
  taskuebergreifender Synchronisationsmechanismus verwendet.

## Modulstruktur und Verantwortlichkeiten

| Modul | Verantwortung |
| --- | --- |
| `main.cpp` | Globale Komponenten, Zustandsautomat sowie Setup-/Loop-Orchestrierung. |
| `PulseAccounting` | Zaehlerdifferenzen, Puffertransfer, S0-Diagnosen und Ueberlaufvorsorge. |
| `CountPin` | GPIO-ISR, Flankenvalidierung und atomare Zaehlerzugriffe. |
| `DatabaseWorker` | Queue- und Task-Lifecycle, serialisierte Datenbankauftraege. |
| `ConsumptionSync` | Batch-Snapshot, Retry-Identitaet und DB-/Buffer-Bestaetigung. |
| `ConsumptionBuffer` | Synchronisierter RAM-/NVS-Arbeitszustand und Snapshot-Verrechnung. |
| `NvsConsumptionStore` | Versionierter NVS-Datensatz, Validierung und Migration. |
| `DatabaseConnection` | MariaDB-/MySQL-Verbindung und atomare SQL-Transaktion. |
| `SerialCommandProcessor` | Nicht-blockierender Parser und Hintergrundempfang fuer USB-Serial. |
| `WirelessConnection` | WLAN-Events, Verbindung und Reconnect-Steuerung. |
| `NtpClock` / `UploadScheduler` | Systemzeit und lokale Upload-Faelligkeit. |
| `NvsStorageMonitor` / `RamStorageMonitor` | Speichermessung und Warnschwellen. |
| `StatusLed` / `StatusWebServer` | LED-Statusausgabe und lokale HTTP-Statusanzeige. |
| `Logging` | Task-sichere, zeitgestempelte serielle Ausgabe. |

Jede Klasse hat eine passende Deklaration unter `include/` und Implementierung
unter `src/`. PlatformIO baut alle `.cpp`-Dateien im `src/`-Verzeichnis.
Gemeinsam genutzte Komponenten werden in `main.cpp` angelegt und per Referenz
an ihre Besitzer/Koordinatoren uebergeben. Neue Hardware- oder Protokoll-
Funktionen sollen in einem eigenen Modul liegen und nicht die zentrale
Schleifenlogik erweitern, sofern sie nicht tatsaechlich Ablaufsteuerung sind.

## Konfigurations- und Persistenzvertrag

`include/Config.h.example` ist die versionierte Vorlage; die lokale
`include/Config.h` enthaelt private WLAN- und DB-Werte und bleibt ignoriert.
Compile-time `static_assert`s sichern unter anderem eindeutige Pins und
Datenbank-IDs, endliche positive Zaehleraufloesung und einen gueltigen
Uploadteiler von 60.

Der NVS-Zustand enthaelt Magic/Formatversion, naechste Sequenz, vier
Verbrauchswerte und ein persistentes Sofort-Upload-Flag. Migrationen muessen
alte Werte erhalten und duerfen ungueltige Datensaetze nicht stillschweigend
ueberschreiben. Das Schema speichert Verbrauch als `DECIMAL(65, 12)` und
verwendet `energy_sync_state` zur idempotenten Verarbeitung der fortlaufenden
Geraetesequenz.

## Entwicklungsumgebung und Build

Voraussetzungen sind PlatformIO Core, die Umgebung `arduino_nano_esp32` und
eine lokale `include/Config.h`. Nuetzliche Befehle:

```text
pio run
pio run --target upload
pio device monitor --baud 115200
pio run --target clean
git diff --check
```

Vor einem Pull Request: Firmware bauen, `git diff --check` ausfuehren und
geaenderte Persistenz-, Retry- und Fehlerpfade gegen RAM- und NVS-Modus
pruefen. Keine echten Zugangsdaten committen. Hinweise zu DB-Installation und
SQL-Auswertung stehen in [`database/README.md`](database/README.md).

GitHub Actions baut die Firmware bei jedem Push und Pull Request mit
`pio run`. Der Workflow unter `.github/workflows/platformio.yml` erstellt
dafuer eine Build-Konfiguration aus `include/Config.h.example`; lokale
Zugangsdaten oder GitHub-Secrets sind fuer den Build nicht erforderlich.

## Fuer Nutzer: Voraussetzungen und Einrichtung

### Hardware und Dienste

- Arduino Nano ESP32 (ESP32-S3) und eine passende Stromversorgung.
- Vier Impulsgeber bzw. Zaehlerausgaenge an den in `Config::Counters`
  festgelegten GPIO-Pins (Vorgabe: 4, 5, 6 und 7).
- WLAN mit Zugang zum MariaDB-/MySQL-Server und zu mindestens einem NTP-Server.
- MariaDB oder MySQL mit InnoDB-Unterstuetzung.
- PlatformIO in Visual Studio Code oder die PlatformIO Core CLI.

Die Eingangspins sind standardmaessig GPIO 4 bis 7, lassen sich aber in
`Config::Counters` einzeln aendern. Die Impulspolaritaet ist mit
`Config::Runtime::pulseActiveHigh` einstellbar: `true` erwartet aktive HIGH-
Pulse mit internem Pulldown, `false` aktive LOW-Pulse mit internem Pullup
(typisch bei S0-Open-Collector-Ausgaengen). Beide Signalflanken werden
ausgewertet, um die Dauer des aktiven Pegels zu messen. Pruefe die Beschaltung
und den Zaehlerausgang, bevor du die Polaritaet einstellst. Waehle nur fuer das
Board verfuegbare GPIOs, verwende jeden Pin nur einmal und stelle sicher, dass
die Signalpegel elektrisch zum ESP32 passen (3,3-V-Logik). Die Datenbank
verwendet die Datenbank-IDs `Config::Counters::databaseId1` bis `databaseId4`
(Vorgabe: 4 bis 7); diese IDs bezeichnen die vier Kanaele und muessen nicht den
GPIO-Nummern entsprechen. Die Datenbank-IDs muessen eindeutig sein. Aendere sie
nur, wenn keine Verbrauchswerte im Puffer ausstehen, damit gepufferte Werte
nicht unter einer neuen ID uebertragen werden.

### Schematische S0-Verdrahtung

Das folgende Prinzipschaltbild zeigt einen Zaehlerkanal. Fuer weitere
Zaehler wird jeweils ein eigener S0-Eingang am Interface-Modul und ein eigener
ESP32-GPIO verwendet. Das Interface-Modul muss fuer S0-Zaehlerausgaenge geeignet
sein und einen 3,3-V-tauglichen Open-Collector-Ausgang bereitstellen:

```text
       Energiezaehler                         S0-Eingangsmodul
   +---------------------+                +------------------------+
   | S0+ o---------------+----------------| S0+ / Pulse input      |
   |                     |                |                        |
   | S0- o---------------+----------------| S0- / Pulse return     |
   +---------------------+                |                        |
                                          |   S0-Strombegrenzung / |
                                          |   geeigneter Eingang   |
                                          |                        |
                                          | OUT (Open Collector) o-+----+---- GPIO
                                          | GND                 o-+----|---- GND
                                          +------------------------+    |
                                                                        |
 ESP32: 3V3 o----------------[ ca. 10 kOhm ]----------------------------+
           (Pull-up; alternativ INPUT_PULLUP im ESP32 verwenden)
```

Die S0-Klemmen des Zaehlerausgangs werden nur mit dem S0-Eingang des dafuer
vorgesehenen Moduls verbunden. Das Modul muss den laut Zaehlerdatenblatt
erforderlichen S0-Strom und die Eingangsspannung bereitstellen bzw. begrenzen.
Auf der ESP32-Seite zieht der Open-Collector-Ausgang den GPIO waehrend des
Impulses nach GND. In dieser gezeigten Schaltung ist der Impuls daher aktiv
LOW; setze dafuer `Config::Runtime::pulseActiveHigh` auf `false`. Der
Firmwarecode verwendet dann `INPUT_PULLUP`. Der externe Pull-up ist bei dieser
Einstellung normalerweise nicht erforderlich; verwende nicht beide Pull-ups
gleichzeitig, ausser das Interface-Datenblatt empfiehlt es.

| Zaehlerkanal | Datenbank-ID | ESP32-Anschluss (Vorgabe) | Konfiguration |
| --- | ---: | --- | --- |
| 1 | 4 | GPIO 4 | `Config::Counters::pin1` |
| 2 | 5 | GPIO 5 | `Config::Counters::pin2` |
| 3 | 6 | GPIO 6 | `Config::Counters::pin3` |
| 4 | 7 | GPIO 7 | `Config::Counters::pin4` |

**Wichtig:** S0-Schnittstellen koennen je nach Zaehler unterschiedliche
Spannungs- und Stromanforderungen haben. Verbinde S0+ oder S0- niemals direkt
mit einem ESP32-GPIO und lege keine S0-Versorgungsspannung an den GPIO.
Pruefe Klemmenbelegung, Polaritaet und elektrische Grenzwerte im Zaehler- und
Interface-Datenblatt. Ein ungeeignetes oder falsch verdrahtetes Interface kann
den ESP32 beschaedigen.

`Config::Runtime::resolutionPulsesPerKWh` gibt die Zaehleraufloesung in
Impulsen pro Kilowattstunde an. Die Firmware speichert Verbrauchswerte in kWh:
bei 1000 Impulsen/kWh entspricht ein gueltiger Impuls 0,001 kWh (1 Wh).
`Config::Runtime::minimumPulseDurationMs` legt die erforderliche Mindestdauer
des aktiven Pegels fest; kuerzere Pulse werden verworfen.
`Config::Runtime::minimumPulseIntervalMs` begrenzt zusaetzlich den
Mindestabstand zwischen gezaehlten Impulsen.


### 1. Datenbank vorbereiten

Fuehre [database/schema.sql](database/schema.sql) mit einem administrativen
MariaDB-/MySQL-Konto auf dem Server aus. Das Skript erstellt die Datenbank
`energy_monitor`, die Zeitreihentabelle `energy_measurements` und den
idempotenten Synchronisationsstatus.

Erstelle danach einen dedizierten Anwendungsbenutzer. Er benoetigt Zugriff auf
`energy_measurements` zum Einfuegen sowie auf `energy_sync_state` zum Lesen,
Einfuegen und Aktualisieren. Rechte sollten auf die verwendeten Tabellen und
den ESP32-Netzwerkstandort begrenzt werden. Die genaue GRANT-Syntax haengt von
der MariaDB-/MySQL-Version und Netzwerkkonfiguration ab.

Weitere Informationen zu Tabellen und Datenbankfunktionen stehen in
[database/README.md](database/README.md).

### 2. Globale Konfiguration anlegen

Kopiere `include/Config.h.example` nach `include/Config.h`. Trage dort zentral
WLAN-Zugangsdaten, Datenbankverbindung sowie Laufzeit-, Intervall- und
Zeiteinstellungen in den passenden Bereichen ein:

- `Config::Wifi`: SSID und WLAN-Passwort.
- `Config::Database`: Host, Port, Datenbank, Benutzer und Passwort.
- `Config::Counters`: GPIO-Pin fuer jeden der vier Impulszaehler.
- `Config::Runtime`: Impulsfaktor, Entprellzeit, Uploadtakt,
   Pufferspeicher (`useNvsConsumptionBuffer`), RAM-/NVS-Schwellen,
   Wiederholungsintervalle, WLAN-Timeouts, Logging-Stufe und serielle Ausgabe.
- `Config::Time`: Zeitzone und NTP-Server.

Die echte `include/Config.h` wird von Git ignoriert, da sie Zugangsdaten
enthalten kann. Nur `Config.h.example` wird versioniert. Die Firmware bricht
beim Kompilieren mit einem Hinweis ab, wenn die lokale Konfiguration fehlt.
Nach Aenderungen an `Config.h` muss die Firmware neu gebaut und hochgeladen
werden; es handelt sich nicht um eine zur Laufzeit geladene Konfiguration.
`Config::Runtime::useNvsConsumptionBuffer = false` waehlt RAM (Standard); bei
`true` wird jeder neue Verbrauchswert im NVS gesichert. Im RAM-Modus gehen noch
nicht hochgeladene Werte bei Stromverlust oder Neustart verloren. Die Upload-
Sequenz bleibt trotzdem im NVS gespeichert.

### 3. Firmware bauen und hochladen

Oeffne das Projektverzeichnis in PlatformIO und waehle die Umgebung
`arduino_nano_esp32`. Die Core-Befehle aus dem Projektverzeichnis lauten:

```text
pio run
pio run --target upload
pio device monitor --baud 115200
```

Waehle bei mehreren angeschlossenen Boards den richtigen seriellen Port in
PlatformIO. Nach dem Start sollten serielle Meldungen fuer den gewaehlten Puffer,
die WLAN-Verbindung und die NTP-Synchronisation erscheinen. Ein Datenbankupload
ist erst moeglich, wenn das SQL-Schema vorbereitet und der ESP32 mit dem Server
verbunden ist.

### 4. Betrieb kontrollieren

Nach dem WLAN-Verbindungsaufbau gibt die serielle Ausgabe die lokale URL des
Status-Dashboards aus. Es zeigt WLAN-Signal, ausstehenden Verbrauch, Heap und
eine naeherungsweise Auslastung der Hauptschleife; die Anzeige aktualisiert
sich alle fuenf Sekunden. Die CPU-Angabe ist keine Messung der gesamten
ESP32-Systemauslastung.

Die serielle Ausgabe zeigt WLAN-Events, Datenbankverbindungsfehler, NTP-Status,
Speicherstand und Upload-Ergebnisse entsprechend der eingestellten Logging-
Stufe. Der freie NVS-Platz oder RAM-Heap wird je nach Puffermodus
standardmaessig einmal pro Minute geprueft. Ein erfolgreicher Datenbank-Commit
bestaetigt das Messintervall und entfernt erst dann den zugehoerigen Puffer.
Nach jedem WLAN-Verbindungsaufbau prueft die Firmware einmal, ob Anmeldung und
Datenbankauswahl funktionieren; bei Fehlern wird die Verbindung bei spaeteren
Uploadversuchen erneut geprueft.

## Projektstruktur

| Pfad | Aufgabe |
| --- | --- |
| `src/main.cpp` | Initialisiert Komponenten und orchestriert Impulserfassung, Netzwerk, NTP und Uploads. |
| `src/DatabaseWorker.cpp`, `include/DatabaseWorker.h` | Isolierter FreeRTOS-Task mit Auftrags- und Ergebnisqueue fuer Datenbankoperationen. |
| `src/PulseAccounting.cpp`, `include/PulseAccounting.h` | Uebertraegt Impulsdifferenzen je Kanal in den Verbrauchspuffer und meldet ISR-Diagnosen. |
| `include/Config.h.example` | Vorlage fuer die lokale, zentrale Konfiguration. |
| `include/Config.h` | Lokale WLAN-, Datenbank- und Laufzeiteinstellungen; wird ignoriert. |
| `src/CountPin.cpp`, `include/CountPin.h` | S0-Impulszaehler: aktive Pulsdauer, Signalpolaritaet, Mindestabstand und Aufloesungsumrechnung. |
| `src/Logging.cpp`, `include/Logging.h` | Zeitgestempelte, task-synchronisierte Protokollierung mit konfigurierbaren Stufen. |
| `src/WirelessConnection.cpp`, `include/WirelessConnection.h` | WLAN-Verbindung, Reconnects, Events und Signalstaerke. |
| `src/StatusLed.cpp`, `include/StatusLed.h` | RGB-Statusanzeige; unabhaengige LED-Aufgabe fuer Blinksignale waehrend blockierender Uploads. |
| `src/StatusWebServer.cpp`, `include/StatusWebServer.h` | Lokales Status-Dashboard und JSON-Endpunkt fuer Betriebswerte. |
| `src/NtpClock.cpp`, `include/NtpClock.h` | NTP-Start, Plausibilitaetspruefung und Europe/Berlin-Systemzeit. |
| `src/UploadScheduler.cpp`, `include/UploadScheduler.h` | Faelligkeitsverwaltung fuer feste Upload-Zeitpunkte. |
| `src/NvsStorageMonitor.cpp`, `include/NvsStorageMonitor.h` | Periodische NVS-Statistik und Schwellwertpruefung. |
| `src/RamStorageMonitor.cpp`, `include/RamStorageMonitor.h` | Periodische Heap-Statistik und RAM-Reservewarnung. |
| `src/ConsumptionBuffer.cpp`, `include/ConsumptionBuffer.h` | Modusneutraler Verbrauchspuffer fuer RAM oder NVS. |
| `include/ConsumptionBufferState.h` | Gemeinsames Datenformat fuer RAM-Zustand und NVS-Checkpoints. |
| `src/NvsConsumptionStore.cpp`, `include/NvsConsumptionStore.h` | NVS-Laden, Migration und Checkpoint-Schreibvorgaenge. |
| `src/ConsumptionSync.cpp`, `include/ConsumptionSync.h` | Zeitreihen-Upload und bestaetigungsabhaengige Pufferloeschung. |
| `src/DatabaseConnection.cpp`, `include/DatabaseConnection.h` | MariaDB-/MySQL-Zugriff und transaktionaler Messintervall-Upload. |
| `database/schema.sql` | Zeitreihen- und Idempotenzschema. |

PlatformIO kompiliert die Quellen aus `src/` automatisch. Bibliotheksabhaengig-
keiten und die fuer ESP32 notwendige Ausschlussliste stehen in
`platformio.ini`. Die `lib_ignore`-Eintraege vermeiden Konflikte mit
WLAN-/Ethernet-Shield-Bibliotheken, die transitiv von der MySQL-Bibliothek
angeboten werden.

Auf dem Dual-Core-Nano-ESP32 laeuft die Arduino-Messschleife weiter auf dem
Arduino-Core. Ein separater FreeRTOS-Task auf Core 0 erledigt blockierende
MariaDB-/MySQL-Verbindungspruefungen und Uploads, damit diese die Verarbeitung
neuer Zaehlerstaende nicht aufhalten. Auftraege und Ergebnisse werden ueber
FreeRTOS-Queues uebergeben; waehrend eines Uploads bleibt die Impulserfassung
per GPIO-Interrupt aktiv. Haupttask, Worker und Status-Webserver greifen
mutex-geschuetzt auf den Verbrauchspuffer zu. Der Worker bestaetigt nach dem
Datenbank-Commit nur den gesendeten Snapshot; waehrend des Uploads neu
hinzugekommene Messwerte bleiben im Puffer. Solange der Worker aktiv ist,
werden alle Datenbankoperationen ueber ihn serialisiert. Kann der Worker oder
der task-sichere Logger nicht gestartet werden, verwendet die Firmware den
bisherigen synchronen Datenbankpfad.

### RGB-Statusanzeige

Die eingebaute RGB-LED zeigt den Betriebszustand mit folgender Prioritaet:

1. **Rot dauerhaft:** WLAN getrennt, Verbrauchspuffer nicht initialisiert oder der
  letzte Datenbankupload ist fehlgeschlagen.
2. **Gelb blinkend:** Messdaten werden gerade an die Datenbank gesendet.
3. **Orange dauerhaft:** Die freie NVS-Kapazitaet oder der freie RAM-Heap liegt
   je nach Puffermodus bei oder unter der konfigurierten Schwelle
   (`minimumFreeNvsPercent` bzw. `minimumFreeRamPercent`, Vorgabe jeweils 10 %).
4. **Blau dauerhaft:** WLAN verbunden, aber die Datenbank ist beim letzten Test
  nicht erreichbar.
5. **Gruen dauerhaft:** WLAN und Datenbank erreichbar und kein hoeher
  priorisierter Zustand aktiv.

Ein fehlgeschlagener Upload bleibt rot angezeigt, bis ein spaeterer Upload
erfolgreich war. Waehrend des Uploads laeuft die LED-Anzeige in einer eigenen
Aufgabe, damit das gelbe Blinksignal auch waehrend der synchronen
Datenbankkommunikation erhalten bleibt. Die Datenbank wird beim WLAN-Aufbau,
nach Uploads und anschliessend gemaess
`Config::Runtime::databaseCheckIntervalMs` (Vorgabe 60 Sekunden) geprueft.

## Ablauf und Ausfallsicherheit

1. Die GPIO-Interrupts messen beide Flanken. Nur Pulse mit passender Polaritaet,
   ausreichender aktiver Dauer und ausreichendem Mindestabstand werden gezaehlt.
2. `PulseAccounting` vergleicht die Zaehlerstaende mit den zuletzt gepufferten
   Staenden und addiert nur neue Impulse als Verbrauchsdifferenz zum
   konfigurierten Puffer. `Config::Runtime::useNvsConsumptionBuffer` waehlt
   zwischen RAM (Standard) und NVS.
3. `NtpClock` setzt die Systemzeit. Die Zeitzone
   `CET-1CEST,M3.5.0/2,M10.5.0/3` schaltet automatisch zwischen Normal- und
   Sommerzeit um.
4. `UploadScheduler` merkt einen faelligen Zeitpunkt vor, bis der Upload
   bestaetigt wurde. Beim Upload wird der Verbrauch jedes Zaehlerkanals seit
   dem letzten erfolgreich gespeicherten Messzeitpunkt als eigener Datensatz
   abgelegt. Der Datenbankserver vergibt fuer alle vier Datensaetze eines
   Batches denselben UTC-Zeitstempel.
5. Ein Batch wird mit ESP32-Geraete-ID und persistenter Sequenznummer
   transaktional eingefuegt. Der Server kann denselben Batch bei einem
   Verbindungsabbruch sicher wiedererkennen und doppelte Messreihen vermeiden.
6. Erst nach erfolgreichem Commit werden die Werte des gesendeten Snapshots
   vom Puffer abgezogen und die Sequenz erhoeht. Bei Fehlern bleibt der Batch
   fuer den naechsten Versuch unveraendert; waehrenddessen neu erfasste Werte
   bleiben zusaetzlich im Puffer.
   Im RAM-Modus gehen noch nicht bestaetigte Werte bei Stromverlust oder Reset
   verloren; im NVS-Modus bleiben sie erhalten.
7. Der Arduino-Haupttask erfasst Zaehlerstaende und betreut WLAN/NTP; ein
   separater Task auf Core 0 fuehrt blockierende Datenbankzugriffe aus. Solange
   ein Upload laeuft, werden neue Pulse weiterhin durch die GPIO-Interrupts
   gezaehlt und vom Hauptloop parallel in den Puffer uebernommen.
   Die Speicherueberwachung laeuft unabhaengig von WLAN. Im NVS-Modus kann
   knapper NVS-Freiplatz einen zusaetzlichen Upload ausloesen. Im RAM-Modus
   meldet sie knappen Heap und schaltet die Status-LED auf Orange; ein Upload
   wird dadurch nicht ausgeloest, da der RAM-Puffer fest dimensioniert ist.
   Jeder Upload benoetigt WLAN und eine erreichbare Datenbank.

Jede Zeile in `energy_measurements` enthaelt Geraete-ID, Zaehler-ID,
Uploadsequenz, `measured_at` und `consumption_kwh`. `measured_at` ist der
gemeinsame UTC-Endzeitpunkt des Uploads; `consumption_kwh` ist der Verbrauch
seit dem vorherigen erfolgreichen Upload desselben Geraets und Zaehlerkanals.
Auch wenn in einem Intervall kein Verbrauch anfiel, speichert die Firmware bei
jedem regulaeren Messzeitpunkt einen Nullwert. Wenn ein Upload durch einen
Ausfall spaeter erfolgt, wird die bis dahin aufgelaufene Energiemenge mit dem
spaeteren Endzeitpunkt gespeichert; die verstrichene Zeit laesst sich mit dem
vorherigen Zeitstempel ermitteln. Beim ersten gespeicherten Messpunkt gibt es
noch keinen vorherigen Datenbankzeitstempel.

Beispiel fuer die Auswertung eines Zaehlerkanals; die vorherige Messzeit ist
damit zugleich der Start des jeweiligen Messintervalls:

```sql
SELECT
    device_id,
    counter_id,
    LAG(measured_at) OVER (
        PARTITION BY device_id, counter_id
        ORDER BY measured_at, sequence
    ) AS interval_start_utc,
    measured_at AS interval_end_utc,
    consumption_kwh
FROM energy_measurements
WHERE counter_id = 4
ORDER BY device_id, measured_at, sequence;
```

Der NVS-Monitor misst freie Eintraege der NVS-Partition, nicht die gesamte
physische Flash-Kapazitaet und nicht den fuer Firmware reservierten Bereich.
Im NVS-Modus ist der Verbrauchspuffer ein fest dimensionierter Datensatz; die
Zahl gespeicherter Schluessel waechst nicht mit den Verbrauchswerten. Im
RAM-Modus werden normale Impulse nicht einzeln ins NVS geschrieben; Sequenzen
und seltene Ueberlauf-Checkpoints bleiben dennoch im NVS gespeichert.

### Ueberlauf- und Wertebereichsschutz

Die Impuls- und Diagnosezaehler verwenden 64-Bit-Zaehler mit Saettigung statt
Wraparound. Eine Million Pulse vor dem Maximalwert wird der aktuelle Stand
dauerhaft im NVS gesichert, der lokale Zaehlwert atomar entlastet und ein
sofortiger Datenbankupload angefordert. Die Uploadanforderung bleibt bei
Netzwerkfehlern und Neustarts gespeichert; nach erfolgreicher
Datenbankbestaetigung wird sie geloescht. Dadurch kann der lokale Zaehler
weiterlaufen, waehrend die Datenbank die Verbrauchsdifferenzen als
Zeitreihenmessungen speichert.
Die `micros()`-Zeitmarken bleiben 32-bittig und werden mit modularer Differenz
ausgewertet, damit der normale Zeitgeber-Wraparound die Pulsdauer nicht
verfaelscht. Fuer die beiden S0-Zeitparameter erzwingt die Firmware ausserdem
einen innerhalb dieses Messbereichs darstellbaren Konfigurationswert.

Messwerte werden als `DECIMAL(65, 12)` gespeichert, also mit bis zu 53 Stellen
vor und 12 Stellen nach dem Dezimalpunkt. Alte kumulierte Daten in
`energy_consumption` werden vom neuen Schema nicht veraendert oder automatisch
in Zeitreihen umgedeutet: ohne historische Messzeitpunkte laesst sich daraus
kein korrekter Intervallverbrauch rekonstruieren. Die bisherige Tabelle bleibt
als Altbestand erhalten; neue Messungen landen in `energy_measurements`.

## Konfiguration

Die zentralen Werte liegen nach dem Kopieren in `include/Config.h`:

- `Config::Runtime::resolutionPulsesPerKWh`: Aufloesung in Impulsen pro kWh; Typenschild oder Datenblatt beachten.
- `Config::Runtime::pulseActiveHigh`: `true` fuer aktive HIGH-, `false` fuer aktive LOW-Pulse.
- `Config::Runtime::minimumPulseDurationMs`: erforderliche Mindestdauer des aktiven Pegels.
- `Config::Runtime::minimumPulseIntervalMs`: Mindestabstand zwischen gezaehlten Impulsen.
- `Config::Counters::pin1` bis `pin4`: GPIO-Zuordnung der vier Impulseingaenge.
- `Config::Runtime::uploadEveryMinutes`: Uploadabstand. Er muss ein positiver Teiler von 60
  sein. `15` ergibt lokale Zeitpunkte wie 15:00, 15:15, 15:30 und 15:45.
- `Config::Runtime::minimumFreeNvsPercent`: Schwelle fuer den zusaetzlichen Speicher-Upload.
- `Config::Runtime::nvsMonitorIntervalMs`: Abstand zwischen NVS-Statistikabfragen.
- `Config::Runtime::minimumFreeRamPercent`: Mindestanteil freien Heaps, bevor die Status-LED auf Speicherwarnung wechselt.
- `Config::Runtime::ramMonitorIntervalMs`: Abstand zwischen RAM-Heap-Messungen.
- `Config::Runtime::uploadRetryMs`: Wartezeit fuer fehlgeschlagene Uploads.
- `Config::Runtime::databaseCheckIntervalMs`: Intervall fuer den Datenbankstatus-Test.
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
Datenbank verfuegbar sind. Die Zeitstempel der Messreihe werden unabhaengig
davon in UTC durch den Datenbankserver vergeben.

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

Im seriellen Monitor koennen zeilenweise Befehle eingegeben werden:

| Befehl | Wirkung |
| --- | --- |
| `HELP` oder `?` | Zeigt alle Befehle. |
| `STOP` | Pausiert die Erfassung, uebernimmt den letzten Zaehlerstand und uebertraegt den Puffer. Bei Fehlern bleibt der Stopp aktiv und der Upload wird wiederholt. |
| `START` | Setzt die Erfassung nach einem Stopp oder KILL fort. |
| `KILL` | Pausiert ohne Flush. Noch nicht gepufferte Impulse und RAM-Puffer koennen verloren gehen. |
| `DEBUG ERROR`, `DEBUG WARNING`, `DEBUG INFO`, `DEBUG DEBUG` oder `DEBUG DEBUG_PLUS` | Aendert die Logstufe bis zum Neustart. |

Beim normalen `STOP` blinkt die orange LED; die RGB-LED zeigt weiterhin ihren
normalen Upload- und Fehlerzustand. Nach bestaetigtem Upload ist der Puffer leer,
RGB geht aus und Orange leuchtet dauerhaft als Flash-Freigabe. `KILL` schaltet
RGB aus und laesst Orange schnell blinken. `START` schaltet Orange aus und gibt
die RGB-Statusanzeige wieder frei. Der serielle Empfang pausiert die
Zaehl-Interrupts sofort. Ein bereits laufender SQL-Aufruf kann nicht sicher
abgebrochen werden und laeuft bis zu seinem Ende weiter; ein Reset waehrenddessen
kann dessen Ergebnis unklar lassen.

## Logging und Fehlerbehebung

Die Logging-Stufen und die zur Laufzeit gewaehlte Mindeststufe sind in
`include/Logging.h` beziehungsweise `src/Logging.cpp` definiert. Die
Ausgabestufen sind `ERROR`, `WARNING`, `INFO`, `DEBUG` und `DEBUG+`.
Die Startstufe stellst du ueber `Config::Runtime::minimumLogLevel` ein; zur
Laufzeit kann sie mit `DEBUG <LEVEL>` geaendert werden. `DEBUG`
protokolliert wichtige Zustandswechsel und erfolgreiche Schritte; `DEBUG+`
enthaelt zusaetzliche WLAN-Details, RSSI-Werte, gepufferte Zaehlerwerte und
Diagnosezaehler verworfener Pulse. Die Interrupt-Service-Routine schreibt
absichtlich nicht direkt auf die serielle Schnittstelle: sie zaehlt verworfene
Pulse und gibt die Diagnose spaeter aus dem Hauptprogramm aus. Dadurch wird die
Zeit im Interrupt kurz gehalten. Serielle Meldungen erscheinen nach NTP mit
lokalem Datum und Uhrzeit. Davor kennzeichnet der Zeitstempel, dass die Uhr noch
nicht synchronisiert ist. Setze die Mindeststufe auf `LOG_INFO` fuer den
Normalbetrieb oder `LOG_DEBUG_PLUS` zur Fehlersuche.

| Bereich | `INFO` / `WARNING` / `ERROR` | `DEBUG` | `DEBUG+` |
| --- | --- | --- | --- |
| WLAN | Verbindung, IP-Adresse, Trennung und Reconnect-Warnungen | Statuswechsel und Reconnect-Ablauf | Stationsereignisse und periodische Signalstaerke |
| Datenbank | Verbindungsfehler, fehlgeschlagene Transaktionen und gespeicherte Messintervalle | Schreibaktionen und Sequenzablauf | erfolgreiche SQL- und Transaktionsschritte |
| NTP und Zeitplan | erste Zeitsynchronisierung und erreichter Messzeitpunkt | SNTP-Start und Initialisierung des Zeitplans | Zeitfensterwechsel und detaillierte Scheduler-Entscheidungen |
| Zaehler/Interrupt | ungueltige Zustands-/Pufferfehler | konfigurierte Zaehler sowie neu erkannte und gepufferte Pulse | ISR-Diagnose fuer verworfene Pulse; Ausgabe erfolgt verzoegert im Hauptprogramm |
| RAM-/NVS-Speicher | Speicherfehler und kritische Reserve | Pufferstart, Batch- und Speicherentscheidungen | Heap-/NVS-Details und Checkpoint-Schreibvorgaenge |

| Symptom | Pruefen |
| --- | --- |
| WLAN verbindet nicht | `Config::Wifi`, SSID, Passwort, Empfang und serielle WLAN-Status-/Eventmeldungen kontrollieren. Der Reconnect wird automatisch wiederholt. |
| NTP-Zeit bleibt aus | Internetzugriff/DNS und NTP-Server in `Config::Time` pruefen. Ohne NTP sind zeitgesteuerte Uploads ausgesetzt. |
| Datenbankverbindung scheitert | `Config::Database`, Serveradresse, Port 3306, Netzwerkroute, Firewall und DB-Berechtigungen pruefen. |
| Messwerte fehlen | `database/schema.sql` ausfuehren, DB-Berechtigungen auf `energy_measurements` pruefen und den UTC-Zeitgeber des Servers kontrollieren. |
| Upload wird wiederholt | Das ist bei Timeout oder verlorener Bestaetigung beabsichtigt. Geraete-ID und Sequenznummer verhindern doppelte Messdatensaetze. Puffer nur nach Analyse manuell loeschen. |
| NVS-Puffer kann nicht geladen werden | Meldung nicht ignorieren: ein ungueltiger Puffer wird nicht automatisch ueberschrieben, um moeglicherweise ungesendete Werte nicht still zu verwerfen. |
| Impulse fehlen | Signalpolaritaet, GPIO-Zuordnung, Mindestdauer des aktiven Pegels und Mindestabstand mit dem Zaehlerdatenblatt vergleichen. |

## Sicherheit und Betriebshinweise

- Die MariaDB-/MySQL-Verbindung des aktuellen Firmwaremoduls verwendet kein
  TLS. Verwende sie nur in einem kontrollierten, vertrauenswuerdigen LAN und
  exponiere den Datenbankport nicht im oeffentlichen Internet.
- Zugangsdaten gehoeren in die ignorierten lokalen Konfigurationsdateien und
  nicht in Commits, Screenshots oder Logs.
- Im NVS-Modus wird der Puffer bei jedem neu erfassten Verbrauchsschritt
  dauerhaft aktualisiert. Das erhoeht die Robustheit gegen Neustarts, beansprucht
  aber Flash-Schreibzyklen. Im RAM-Modus werden normale Impulse nicht einzeln
  gespeichert; Sequenzstatus und seltene Ueberlauf-Checkpoints bleiben im NVS.
  Noch nicht hochgeladene RAM-Werte gehen bei Neustart oder Stromausfall verloren.
- Bei Stromausfall waehrend eines einzelnen NVS-Schreibvorgangs haengt die
  Wiederherstellbarkeit von der NVS-/Flash-Implementierung ab. Die
  Batch-Sequenzierung schuetzt vor doppelter DB-Verbuchung, ersetzt aber keine
  externe unterbrechungsfreie Stromversorgung.
- NTP benoetigt Netzwerkzugriff. Die Systemuhr wird nicht als batteriegepufferte
  Echtzeituhr behandelt.
