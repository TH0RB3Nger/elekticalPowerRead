# MariaDB/MySQL setup

1. Run the complete `schema.sql` on the MariaDB/MySQL server to create the
   database, time-series table, and idempotent upload state. If executing only
   the `CREATE TABLE` statements in phpMyAdmin, first run the `CREATE DATABASE`
   statement; the tables are explicitly created in `energy_monitor`. Existing
   cumulative values in `energy_consumption` are left untouched; they cannot be
   accurately converted into interval readings without historical timestamps.
2. Copy `include/Config.h.example` to `include/Config.h` and set the database
   values in `Config::Database`. The real configuration file, which also
   contains Wi-Fi and runtime settings, is excluded from Git.
3. Create a dedicated database user with only the permissions the firmware
   needs: INSERT on `energy_measurements`, plus SELECT, INSERT, and UPDATE on
   `energy_sync_state`. Allow connections only from the ESP32's local network.
4. Keep the database on a trusted local network. This firmware connects
   directly without TLS; do not expose the database port to the public Internet.

`DatabaseConnection::addConsumptionBatch(deviceId, sequence, records, count)`
inserts one reading for each of the four counter channels in a single
transaction. Each row stores the device, counter, upload sequence, measured
kWh, and the database server's shared UTC timestamp for that batch.

Valid counter IDs are the fixed channel IDs `4` through `7`; GPIO assignments
are independently configurable. The main loop records only new pulses into a
configurable RAM or ESP32 NVS buffer (RAM by default). In RAM mode, unuploaded
values are lost on power loss or reset. Each database row records kWh measured
since the previous successful upload for that device and counter. The configured
resolution is the number of pulses per kWh, so 1000 pulses/kWh add 0.001 kWh
(1 Wh) per valid pulse. NTP sets the scheduler clock to Europe/Berlin time with
automatic summer-time changes; with the default 15-minute interval, uploads
occur on quarter-hour boundaries (15:00, 15:15, 15:30, ...), including zero-kWh
rows when a counter did not consume energy during that interval. The database timestamp is generated in
UTC by the server and shared by all four rows in a batch. In NVS mode, if free
capacity reaches the configured threshold (10% by default), a pending batch is
uploaded immediately instead of waiting for the next boundary. This NVS monitor is
disabled in RAM mode. The RAM monitor reports free, total, and minimum heap;
falling below its configured reserve threshold raises a warning but does not
trigger an upload because uploading does not shrink the fixed-size RAM buffer.
Monitor intervals and thresholds are configured in `Config::Runtime` in
`include/Config.h`.

The buffer is cleared only after the transaction succeeds. A per-device
sequence stored in `energy_sync_state` makes retries safe if the database
committed a batch but its response was lost; this table stores only one sequence
row per ESP32. `energy_measurements` stores time, counter ID, device ID,
sequence, and `DECIMAL(65, 12)` kWh values. The timestamp is the interval end;
if connectivity delays an upload, the interval and consumption value span
until that delayed upload is committed.

Example query with the previous measurement timestamp shown as the interval
start (MySQL 8+/MariaDB versions with window-function support):

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

The ESP32 checkpoints and resets its local 64-bit pulse counter before it can
wrap. The corresponding NVS upload request is persistent and bypasses the
regular quarter-hour schedule. If the database is unavailable, the checkpoint
remains in NVS and the firmware retries when the connection is available.

The NVS monitor reports available NVS key-value entries, not the total physical
flash chip or the firmware partition. In NVS mode, the pending buffer is a
fixed-size record and does not grow as consumption accumulates. In RAM mode,
normal pulse updates do not write pending values to NVS; sequence status and
rare overflow checkpoints are still persisted.

In NVS mode, the buffer is written when new pulses are counted. This preserves
pending consumption across a reboot, but frequent pulse writes use flash
endurance. Adjust the persistence strategy if the meter produces a very high
pulse rate.
NTP requires network access to the configured time server; Internet access is
needed only when using a public time server.
