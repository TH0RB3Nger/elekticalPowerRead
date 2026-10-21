# Elektical Power Read – Anwenderhandbuch

Firmware fuer den Arduino Nano ESP32 zum Erfassen von bis zu vier S0-
Stromzaehlern und zum Uebertragen der Verbrauchswerte an MariaDB/MySQL.
Messwerte werden als kWh-Intervalle gespeichert. Der Standard-Upload erfolgt
alle 15 Minuten zur lokalen Viertelstunde; die Datenbank vergibt den UTC-
Zeitstempel fuer jedes Messintervall.

Dieses Dokument fuehrt durch Hardware, Einrichtung und Bedienung. Technische
Details zu Softwareaufbau, Tasks, Synchronisation und Entwicklung stehen im
[Entwicklerhandbuch](README_DEVELOPERS.md).

## Voraussetzungen

- Arduino Nano ESP32 (ESP32-S3).
- Bis zu vier Stromzaehler mit S0-Ausgang.
- Ein geeignetes S0-Interface, das den Zaehlerausgang elektrisch vom ESP32
  entkoppelt und einen 3,3-V-tauglichen Open-Collector-Ausgang bereitstellt.
- WLAN mit Verbindung zum MariaDB-/MySQL-Server und zu einem NTP-Server.
- MariaDB oder MySQL mit InnoDB-Unterstuetzung.
- PlatformIO in Visual Studio Code oder PlatformIO Core CLI.

## Sicherheit und Verdrahtung

**Verbinde S0+ oder S0- niemals direkt mit einem ESP32-GPIO.** S0-Ausgaenge
koennen eine andere Spannung und einen anderen Strom als ein GPIO benoetigen.
Verwende ein dafuer vorgesehenes Interface und pruefe dessen Beschaltung sowie
die Datenblaetter von Zaehler und Interface. Ein falscher Anschluss kann das
Board beschaedigen.

Bei einem typischen Open-Collector-Interface zieht der Ausgang den GPIO
waehrend des Impulses auf GND. Das ist ein aktiver LOW-Impuls und entspricht
der Beispielkonfiguration `pulseActiveHigh = false` mit internem Pull-up.
Verwende keinen zusaetzlichen Pull-up, wenn das Interface diesen nicht
erfordert.

Voreingestellte GPIOs und Datenbank-Kanal-IDs:

| Kanal | GPIO | Datenbank-ID |
| --- | ---: | ---: |
| 1 | 4 | 4 |
| 2 | 5 | 5 |
| 3 | 6 | 6 |
| 4 | 7 | 7 |

Die GPIO-Pins muessen auf dem Board verfuegbar und eindeutig sein. Auch die
Datenbank-IDs muessen eindeutig bleiben. Aendere die Kanal-IDs nur, wenn keine
Verbrauchswerte im Puffer ausstehen.

## Datenbank einrichten

1. Fuehre [`database/schema.sql`](database/schema.sql) mit einem administrativen
   MariaDB-/MySQL-Konto aus.
2. Lege einen dedizierten Datenbankbenutzer an. Er benoetigt `INSERT` auf
   `energy_measurements` und `SELECT`, `INSERT`, `UPDATE` auf
   `energy_sync_state`.
3. Beschraenke dessen Zugriff auf die benoetigten Tabellen und das lokale
   Netzwerk des ESP32.
4. Trage Datenbankserver, Port, Datenbank und Benutzer in der lokalen
   Konfiguration ein.

Die Firmware-Verbindung verwendet kein TLS. Betreibe die Datenbank deshalb
nur in einem vertrauenswuerdigen lokalen Netz und mache Port 3306 nicht aus
dem oeffentlichen Internet erreichbar. Hinweise zum Schema stehen in
[`database/README.md`](database/README.md).

## Konfiguration

Kopiere `include/Config.h.example` nach `include/Config.h` und passe dort an:

- `Config::Wifi`: WLAN-Name und Passwort.
- `Config::Database`: Host, Port, Datenbank, Benutzer und Passwort.
- `Config::Counters`: GPIOs und Datenbank-IDs der vier Kanaele.
- `Config::Runtime::resolutionPulsesPerKWh`: Zaehleraufloesung laut
  Typenschild/Datenblatt. Bei 1000 Impulsen/kWh entspricht ein Impuls 1 Wh.
- `Config::Runtime::pulseActiveHigh`: `false` fuer typische aktive LOW-
  Open-Collector-S0-Ausgaenge; Beschaltung vorher pruefen.
- `minimumPulseDurationMs` und `minimumPulseIntervalMs`: Filter fuer Stoer-
  und Fehlpulse.
- `uploadEveryMinutes`: Uploadintervall als positiver Teiler von 60.
- `useNvsConsumptionBuffer`: `false` puffert Verbrauch im RAM, `true` sichert
  ihn nach jeder Aenderung im NVS.
- Speicher-, WLAN-, Wiederholungs- und Logging-Einstellungen unter
  `Config::Runtime`.
- Zeitzone und Zeitserver unter `Config::Time`.

Die lokale `include/Config.h` kann Zugangsdaten enthalten und wird daher nicht
versioniert. Aenderungen daran erfordern einen neuen Firmware-Build. Im
RAM-Modus gehen noch nicht hochgeladene Verbrauchswerte bei Stromausfall oder
Neustart verloren. Im NVS-Modus bleiben sie erhalten, allerdings beansprucht
jeder neue Verbrauchswert Flash-Schreibzyklen.

## Firmware installieren

Oeffne das Projekt in PlatformIO, waehle `arduino_nano_esp32` und fuehre im
Projektverzeichnis aus:

```text
pio run
pio run --target upload
pio device monitor --baud 115200
```

Nach dem Start sollte die serielle Ausgabe den Puffer-Modus, WLAN-Status und
die NTP-Synchronisierung melden. Bei erfolgreicher WLAN-Verbindung wird auch
die lokale Adresse des Status-Dashboards ausgegeben.

## Betrieb und serielle Befehle

Das Dashboard zeigt WLAN-Signalstaerke, ausstehenden Verbrauch, Heap und die
naeherungsweise Last der Hauptschleife. Es aktualisiert sich alle fuenf
Sekunden; die CPU-Angabe ist keine Gesamtmessung des ESP32.

| Befehl | Wirkung |
| --- | --- |
| `HELP` oder `?` | Zeigt die verfuegbaren Befehle. |
| `STOP` | Pausiert die Impulserfassung, uebernimmt den letzten Zaehlerstand und wartet, bis der Puffer erfolgreich uebertragen wurde. |
| `START` | Setzt die Erfassung nach `STOP` oder `KILL` fort. |
| `KILL` | Stoppt sofort ohne Puffer-Upload. Nicht gepufferte Impulse und RAM-Werte koennen verloren gehen. |
| `DEBUG ERROR`, `DEBUG WARNING`, `DEBUG INFO`, `DEBUG DEBUG`, `DEBUG DEBUG_PLUS` | Aendert die Logging-Stufe bis zum Neustart. |

Die Status-LEDs zeigen den Betriebszustand:

- **RGB Rot:** WLAN getrennt, Puffer nicht initialisiert oder Uploadfehler.
- **RGB Gelb blinkend:** Datenbankupload laeuft.
- **RGB Orange:** Speicherreserve ist kritisch.
- **RGB Blau:** WLAN verbunden, Datenbank zuletzt nicht erreichbar.
- **RGB Gruen:** WLAN und Datenbank erreichbar.
- **Orange blinkend:** `STOP` wartet auf einen erfolgreichen Upload.
- **Orange dauerhaft:** `STOP` abgeschlossen; Puffer leer.
- **Orange schnell blinkend:** `KILL` aktiv.

Ein fehlgeschlagener Upload wird mit dem unveraenderten Batch wiederholt.
Sequenznummern verhindern doppelte Datenbankeintraege, wenn ein Commit
erfolgreich war, aber die Bestaetigung verloren ging.

## Fehlerbehebung

| Symptom | Pruefen |
| --- | --- |
| WLAN verbindet nicht | SSID, Passwort, Empfang und serielle WLAN-Meldungen pruefen. Die Firmware versucht automatisch erneut zu verbinden. |
| NTP-Zeit bleibt unsynchronisiert | Internet-/DNS-Zugriff und NTP-Server in `Config::Time` pruefen. Zeitgesteuerte Uploads benoetigen eine plausible Uhrzeit. |
| Datenbank nicht erreichbar | Host, Port 3306, Netzwerkroute, Firewall, Zugangsdaten und Berechtigungen pruefen. |
| Messwerte fehlen | Schema und Rechte auf beide Tabellen pruefen. Ein Upload speichert kWh seit dem vorherigen erfolgreichen Upload. |
| Upload wird wiederholt | Netzwerk- oder Datenbankfehler pruefen. Puffer nicht manuell loeschen; Wiederholungen sind fuer sichere Zustellung vorgesehen. |
| Impulse fehlen | Signalpegel, Interface, GPIO-Zuordnung, Polaritaet, Mindestdauer und Mindestabstand mit dem Zaehlerdatenblatt vergleichen. |

## Weitere Informationen

- [Entwicklerhandbuch: Architektur, Datenfluss, Threading und Build](README_DEVELOPERS.md)
- [Datenbankschema und SQL-Hinweise](database/README.md)
