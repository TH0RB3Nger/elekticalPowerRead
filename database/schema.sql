CREATE DATABASE IF NOT EXISTS energy_monitor
    CHARACTER SET utf8mb4
    COLLATE utf8mb4_unicode_ci;

USE energy_monitor;

CREATE TABLE IF NOT EXISTS energy_measurements (
    device_id CHAR(12) NOT NULL,
    counter_id TINYINT UNSIGNED NOT NULL,
    sequence BIGINT UNSIGNED NOT NULL,
    measured_at DATETIME(6) NOT NULL,
    consumption_kwh DECIMAL(65, 12) NOT NULL,
    PRIMARY KEY (device_id, sequence, counter_id),
    INDEX idx_energy_measurements_time (measured_at, counter_id),
    INDEX idx_energy_measurements_device_time (device_id, counter_id, measured_at),
    CONSTRAINT chk_energy_measurement_counter_id CHECK (counter_id BETWEEN 4 AND 7),
    CONSTRAINT chk_energy_measurement_nonnegative CHECK (consumption_kwh >= 0)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS energy_sync_state (
    device_id CHAR(12) NOT NULL,
    last_sequence BIGINT UNSIGNED NOT NULL DEFAULT 0,
    updated_at TIMESTAMP NOT NULL
        DEFAULT CURRENT_TIMESTAMP
        ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (device_id)
) ENGINE=InnoDB;
