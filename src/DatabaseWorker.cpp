#include "DatabaseWorker.h"

#include "Logging.h"

namespace {
struct DatabaseCommand {
    DatabaseCommandType type;
};
}

DatabaseWorker::DatabaseWorker(
    DatabaseConnection& database,
    ConsumptionSync& consumptionSync
)
    : _database(database),
      _consumptionSync(consumptionSync) {}

/// Erstellt zwei Ein-Element-Queues und startet genau einen Worker-Task.
bool DatabaseWorker::begin() {
    if (_active) {
        return true;
    }

    _commandQueue = xQueueCreate(1, sizeof(DatabaseCommand));
    _resultQueue = xQueueCreate(1, sizeof(DatabaseResult));
    if (_commandQueue == nullptr || _resultQueue == nullptr) {
        if (_commandQueue != nullptr) {
            vQueueDelete(_commandQueue);
            _commandQueue = nullptr;
        }
        if (_resultQueue != nullptr) {
            vQueueDelete(_resultQueue);
            _resultQueue = nullptr;
        }
        logError("Queues fuer den Datenbank-Worker konnten nicht angelegt werden");
        return false;
    }

    if (xTaskCreatePinnedToCore(
            taskEntry,
            "database_worker",
            8192,
            this,
            1,
            &_taskHandle,
            0) != pdPASS) {
        vQueueDelete(_commandQueue);
        vQueueDelete(_resultQueue);
        _commandQueue = nullptr;
        _resultQueue = nullptr;
        _taskHandle = nullptr;
        logError("Datenbank-Worker auf Core 0 konnte nicht gestartet werden");
        return false;
    }

    _active = true;
    logInfo("Datenbank-Worker auf Core 0 gestartet; Messschleife bleibt entkoppelt");
    return true;
}

bool DatabaseWorker::isActive() const {
    return _active;
}

bool DatabaseWorker::isBusy() const {
    return _busy;
}

bool DatabaseWorker::requestConnectionCheck() {
    return enqueue(DatabaseCommandType::CheckConnection);
}

bool DatabaseWorker::requestUpload() {
    return enqueue(DatabaseCommandType::UploadPending);
}

/// Ein Worker-Auftrag bleibt bis zur Ergebnisabholung exklusiv belegt.
bool DatabaseWorker::enqueue(DatabaseCommandType type) {
    if (!_active || _busy) {
        return false;
    }

    const DatabaseCommand command{type};
    if (xQueueSend(_commandQueue, &command, 0) != pdPASS) {
        logError("Datenbankauftrag konnte nicht in die Worker-Queue eingereiht werden");
        return false;
    }
    _busy = true;
    return true;
}

bool DatabaseWorker::pollResult(DatabaseResult& result) {
    if (!_active || xQueueReceive(_resultQueue, &result, 0) != pdPASS) {
        return false;
    }
    _busy = false;
    return true;
}

void DatabaseWorker::taskEntry(void* parameter) {
    static_cast<DatabaseWorker*>(parameter)->run();
}

/// Bearbeitet nur jeweils einen Befehl und meldet dessen Ergebnis zurueck.
void DatabaseWorker::run() {
    DatabaseCommand command{};
    while (true) {
        if (xQueueReceive(_commandQueue, &command, portMAX_DELAY) != pdPASS) {
            logError("Datenbank-Worker konnte keinen Auftrag aus der Queue lesen");
            continue;
        }

        const bool succeeded =
            command.type == DatabaseCommandType::CheckConnection
                ? _database.checkConnection()
                : _consumptionSync.uploadPending();
        const DatabaseResult result{command.type, succeeded};
        if (xQueueSend(_resultQueue, &result, portMAX_DELAY) != pdPASS) {
            logError("Datenbank-Worker konnte kein Ergebnis zurueckmelden");
        }
    }
}
