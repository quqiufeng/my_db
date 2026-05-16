#include "mydb_internal.h"

const char* db_errstr(db_t db) {
    if (!db) return "null database handle";
    db_instance_t* inst = (db_instance_t*)db;
    return inst->error_msg;
}

void db_set_error(db_instance_t* db, int code, const char* fmt, ...) {
    db->last_error = code;
    va_list args;
    va_start(args, fmt);
    vsnprintf(db->error_msg, sizeof(db->error_msg), fmt, args);
    va_end(args);
}
