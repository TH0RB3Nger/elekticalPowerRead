# Datenbank: Einrichtung und Datenmodell

Diese Anleitung beschreibt die MariaDB-/MySQL-Seite der Firmware. Die
Geraeteinstallation und Verdrahtung sind in der [Anwender-README](../README.md)
beschrieben. Softwarearchitektur, Datenfluss und Task-Synchronisation stehen
im [Entwicklerhandbuch](../README_DEVELOPERS.md).

## Schema installieren

Fuehre [`schema.sql`](schema.sql) einmal mit einem administrativen
MariaDB-/MySQL-Konto aus. Das Skript erstellt die Datenbank `energy_monitor`
und folgende Tabellen:

- `energy_measurements`: Zeitreihenwerte je Geraet, Kanal und Uploadsequenz.
- `energy_sync_state`: letzter bestaetigter Upload je Geraet.

Das Schema aendert oder loescht vorhandene kumulierte Werte in
`energy_consumption` nicht. Ohne historische Zeitstempel lassen sich daraus
keine korrekten Intervallmessungen rekonstruieren.

## Firmware-Benutzer

Erstelle einen eigenen Benutzer fuer die Firmware und begrenze ihn auf das
lokale Netzwerk des ESP32. Er benoetigt nur:

- `INSERT` auf `energy_measurements`.
- `SELECT`, `INSERT` und `UPDATE` auf `energy_sync_state`.

Trage Host, Port, Datenbankname, Benutzer und Passwort unter
`Config::Database` in `include/Config.h` ein. Die lokale Konfiguration wird
nicht versioniert und darf nicht committet werden.

**Netzwerksicherheit:** Die Firmwareverbindung verwendet kein TLS. Betreibe
die Datenbank nur in einem vertrauenswuerdigen Netz und exponiere den
Datenbankport nicht gegenueber dem oeffentlichen Internet.

## Datenmodell und Upload

Ein Upload enthaelt je einen Datensatz fuer alle vier Zaehlerkanaele. Die
Firmware sendet diese Datensaetze zusammen mit einer stabilen Geraete-ID und
einer fortlaufenden Sequenznummer. Der Datenbankserver speichert die Zeilen in
einer Transaktion und vergibt allen vier denselben UTC-Zeitstempel.

| Spalte | Bedeutung |
| --- | --- |
| `device_id` | Zwoelfstellige Kennung aus der eFuse-MAC des ESP32. |
| `counter_id` | Konfigurierte Kanal-ID; standardmaessig 4 bis 7. |
| `sequence` | Fortlaufende Uploadsequenz je Geraet. |
| `measured_at` | Gemeinsamer UTC-Endzeitpunkt der vier Messwerte. |
| `consumption_kwh` | Verbrauch in kWh seit dem vorherigen erfolgreichen Upload. |

`energy_sync_state` enthaelt genau einen Sequenzstand je Geraet. Dadurch kann
ein Upload nach einer verlorenen Antwort gefahrlos wiederholt werden: Ist die
Sequenz bereits verbucht, bestaetigt die Datenbank den Wiederholungsversuch,
ohne doppelte Zeitreihenwerte anzulegen.

Die Firmware zieht nach erfolgreichem Commit nur die Werte des uebertragenen
Snapshots vom lokalen Puffer ab. Messwerte, die waehrend des Datenbankzugriffs
neu erfasst wurden, bleiben fuer den naechsten Upload erhalten. Details zum
Queue-Worker und den Synchronisationsregeln stehen im
[Entwicklerhandbuch](../README_DEVELOPERS.md#datenfluss-und-threading).

## Intervall und Zeitstempel

Die lokale NTP-Uhr plant Uploads standardmaessig auf Viertelstundenmarken in
`Europe/Berlin`. Bei einer Aufloesung von 1000 Impulsen/kWh addiert ein
gueltiger Impuls 0,001 kWh (1 Wh). Auch Kanaele ohne Verbrauch werden bei einem
regulaeren Upload mit einem Nullwert gespeichert.

`measured_at` ist der UTC-Endzeitpunkt des Uploads, nicht der Beginn des
Intervalls. Der Start ergibt sich aus dem vorherigen Datensatz des gleichen
Geraets und Kanals. Bei spaeter Uebertragung wegen Netzwerkausfall umfasst der
Wert den Verbrauch bis zum Zeitpunkt des erfolgreichen Uploads.

Beispiel fuer eine SQL-Auswertung mit Intervallstart und -ende (MySQL 8+ bzw.
MariaDB mit Window-Function-Unterstuetzung):

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

## Puffer- und Speicherverhalten

Der Geraetepuffer ist standardmaessig RAM-basiert. Nicht bestaetigte
Verbrauchswerte gehen dann bei Stromausfall oder Neustart verloren. Im
konfigurierbaren NVS-Modus werden neue Verbrauchswerte persistent gesichert;
das erhoeht die Ausfallsicherheit, beansprucht aber Flash-Schreibzyklen.

Ein kritischer freier NVS-Anteil kann im NVS-Modus einen vorgezogenen Upload
ausloesen. Im RAM-Modus meldet die Firmware eine kritische Heap-Reserve, loest
dadurch aber keinen Upload aus. Sequenzstatus und seltene Ueberlauf-
Checkpoints werden in beiden Modi im NVS gesichert.

Der NVS-Monitor misst verfuegbare NVS-Eintraege und nicht den gesamten
physischen Flash oder den fuer Firmware reservierten Bereich. Das Verbrauchs-
NVS-Datenformat ist ein fester Datensatz und waechst nicht mit der Zahl der
Impulse.

## Konfiguration und Betrieb

Uploadintervall, Zaehleraufloesung, Puffermodus, NVS-/Heap-Schwellen und
Pruefintervalle stehen in `Config::Runtime` der lokalen `include/Config.h`.
Die Datenbankzeit wird nicht durch die lokale Zeitzone beeinflusst: SQL-
Zeitstempel werden durch den Server in UTC vergeben.

Die Firmware benoetigt Netzwerkzugriff zum konfigurierten NTP-Server; Internet
ist nur erforderlich, wenn ein oeffentlicher Zeitserver verwendet wird.
