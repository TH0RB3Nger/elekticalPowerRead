# MariaDB/MySQL setup

1. Run `schema.sql` on the MariaDB/MySQL server to create the database and
   initialize the four counter rows.
2. Copy `include/Config.h.example` to `include/Config.h` and set the database
   values in `Config::Database`. The real configuration file, which also
   contains Wi-Fi and runtime settings, is excluded from Git.
3. Create a dedicated database user with only the permissions the firmware
   needs: SELECT, INSERT, UPDATE, and DELETE on `energy_consumption`, plus
   SELECT, INSERT, and UPDATE on `energy_sync_state`. Allow connections only
   from the ESP32's local network.
4. Keep the database on a trusted local network. This firmware connects
   directly without TLS; do not expose the database port to the public Internet.

`DatabaseConnection` provides these operations:

- `readConsumption(id, value)` reads one counter's cumulative total.
- `readAll(records, capacity, count)` reads all stored counters.
- `addConsumption(id, amount)` adds an amount, creating the counter row if
  necessary.
- `setConsumption(id, total)` inserts or replaces a cumulative total.
- `deleteCounter(id)` removes a counter row.
- `addConsumptionBatch(deviceId, sequence, records, count)` applies one
  persistent upload batch atomically and only once.

Valid counter IDs are the GPIO numbers `4` through `7`. The main loop records
only new pulses into an ESP32 NVS flash buffer. NTP sets the clock to
Europe/Berlin time with automatic summer-time changes; uploads occur on
quarter-hour boundaries (15:00, 15:15, 15:30, ...). If free capacity in the
NVS partition falls to 10% or lower, a pending batch is uploaded immediately
instead of waiting for the next boundary. The interval and free-space threshold
are constants in `Config::Runtime` in `include/Config.h`.

The buffer is cleared only after the transaction succeeds and a follow-up query
confirms that all four database rows are available. A per-device sequence
stored in `energy_sync_state` makes retries safe if the database committed a
batch but its response was lost; this table stores only one sequence row per
ESP32.

The monitor reports available NVS key-value entries, not the total physical
flash chip or the firmware partition. The pending buffer is a fixed-size NVS
record and does not grow as consumption accumulates.

The flash buffer is written when new pulses are counted. This preserves pending
consumption across a reboot, but frequent pulse writes use flash endurance.
Adjust the persistence strategy if the meter produces a very high pulse rate.
NTP requires internet access to one of the configured time servers.
