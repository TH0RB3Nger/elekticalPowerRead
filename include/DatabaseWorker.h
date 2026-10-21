#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "ConsumptionSync.h"
#include "DatabaseConnection.h"

enum class DatabaseCommandType : uint8_t {
    CheckConnection,
    UploadPending
};

struct DatabaseResult {
    DatabaseCommandType type;
    bool succeeded;
};

/**
 * @brief Fuehrt Datenbankzugriffe einzeln in einem eigenen FreeRTOS-Task aus.
 *
 * Der aufrufende Haupttask kommuniziert ausschliesslich ueber die Queue-API
 * mit dem Worker. So wird die gemeinsame MySQL-Verbindung nie parallel benutzt.
 */
class DatabaseWorker {
public:
    DatabaseWorker(
        DatabaseConnection& database,
        ConsumptionSync& consumptionSync
    );

    /// Erstellt die Queues und startet den Datenbank-Task einmalig.
    bool begin();

    /// Meldet, ob der Worker erfolgreich gestartet wurde.
    bool isActive() const;

    /// Meldet, ob ein Auftrag noch nicht im Haupttask verarbeitet wurde.
    bool isBusy() const;

    /// Stellt eine Verbindungskontrolle ein, sofern der Worker frei ist.
    bool requestConnectionCheck();

    /// Stellt einen Upload ein, sofern der Worker frei ist.
    bool requestUpload();

    /// Holt ein fertiges Ergebnis ab und gibt den Worker wieder frei.
    bool pollResult(DatabaseResult& result);

private:
    static void taskEntry(void* parameter);
    void run();
    bool enqueue(DatabaseCommandType type);

    DatabaseConnection& _database;
    ConsumptionSync& _consumptionSync;
    QueueHandle_t _commandQueue = nullptr;
    QueueHandle_t _resultQueue = nullptr;
    TaskHandle_t _taskHandle = nullptr;
    bool _active = false;
    bool _busy = false;
};
