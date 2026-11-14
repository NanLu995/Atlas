#include "db/db_export.h"
#include "db_sqlite/sqlite_database.h"

ATLAS_DB_BACKEND_API atlas::IDatabase* AtlasCreateDatabaseV2() {
  return new atlas::SqliteDatabase();
}
