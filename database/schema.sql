CREATE DATABASE IF NOT EXISTS energy_monitor
    CHARACTER SET utf8mb4
    COLLATE utf8mb4_unicode_ci;

USE energy_monitor;

CREATE TABLE IF NOT EXISTS energy_consumption (
    counter_id TINYINT UNSIGNED NOT NULL,
    total_consumption DOUBLE NOT NULL DEFAULT 0,
    updated_at TIMESTAMP NOT NULL
        DEFAULT CURRENT_TIMESTAMP
        ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (counter_id),
    CONSTRAINT chk_energy_counter_id CHECK (counter_id BETWEEN 4 AND 7),
    CONSTRAINT chk_energy_consumption_nonnegative CHECK (total_consumption >= 0)
) ENGINE=InnoDB;

ALTER TABLE energy_consumption ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS energy_sync_state (
    device_id CHAR(12) NOT NULL,
    last_sequence BIGINT UNSIGNED NOT NULL DEFAULT 0,
    updated_at TIMESTAMP NOT NULL
        DEFAULT CURRENT_TIMESTAMP
        ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (device_id)
) ENGINE=InnoDB;

INSERT INTO energy_consumption (counter_id, total_consumption)
VALUES (4, 0), (5, 0), (6, 0), (7, 0)
ON DUPLICATE KEY UPDATE counter_id = VALUES(counter_id);
