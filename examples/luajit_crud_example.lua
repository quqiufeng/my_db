#!/usr/bin/env luajit
-- 简化版 CRUD 示例
local ffi = require("ffi")
local os = require("os")

local RTLD_NOW = 0x00002
local RTLD_DEEPBIND = 0x00008
local _lib = ffi.load("./libmydb.so", RTLD_NOW + RTLD_DEEPBIND)

ffi.cdef[[
    typedef struct db_instance* db_t;
    typedef struct db_table*    table_t;
    typedef uint64_t            rowid_t;
    
    enum {
        DB_TYPE_INT32 = 0, DB_TYPE_INT64 = 1, DB_TYPE_UINT64 = 2,
        DB_TYPE_FLOAT = 3, DB_TYPE_DOUBLE = 4, DB_TYPE_STRING = 5,
        DB_TYPE_VARSTRING = 6, DB_TYPE_BOOL = 7,
    };
    
    typedef struct {
        const char* name;
        size_t      offset;
        size_t      size;
        int         type;
    } db_field_def_t;
    
    typedef struct {
        size_t      field_offset;
        size_t      field_size;
        const void* value;
        int         op;
    } db_condition_t;
    
    db_t db_open(const char* db_dir, size_t pool_size);
    void db_close(db_t db);
    int  db_sync(db_t db);
    table_t db_table_register(db_t db, const char* name, size_t row_size,
                              const db_field_def_t* fields, size_t field_count);
    rowid_t db_insert(table_t table, const void* row, size_t row_size);
    size_t  db_batch_insert(table_t table, const void* rows, size_t row_size, size_t count);
    int     db_update(table_t table, rowid_t id, const void* row, size_t row_size);
    int     db_delete(table_t table, rowid_t id);
    const char* db_select_by_pk_json(table_t table, rowid_t id);
    const char* db_select_all_json(table_t table);
    const char* db_select_where_json(table_t table,
                                     const db_condition_t* conditions,
                                     size_t condition_count);
    void db_json_free(const char* json);
    size_t db_table_count(table_t table);
    size_t db_table_compact(table_t table);
    int db_begin(db_t db);
    int db_commit(db_t db);
    int db_rollback(db_t db);
    
    typedef struct {
        int     id;
        char    name[32];
        char    email[64];
        int     age;
        double  score;
    } user_t;
]]

local function cstring_copy(dest, src, max_len)
    max_len = max_len or ffi.sizeof(dest)
    local src_len = #src
    local len = math.min(src_len, max_len - 1)
    ffi.copy(dest, src, len)
    dest[len] = 0
end

os.execute("rm -rf /tmp/mydb_crud_simple")

local db = _lib.db_open("/tmp/mydb_crud_simple", 10 * 1024 * 1024)

local fields = ffi.new("db_field_def_t[5]")
fields[0] = {"id",    ffi.offsetof("user_t", "id"),    4,  0}
fields[1] = {"name",  ffi.offsetof("user_t", "name"),  32, 5}
fields[2] = {"email", ffi.offsetof("user_t", "email"), 64, 5}
fields[3] = {"age",   ffi.offsetof("user_t", "age"),   4,  0}
fields[4] = {"score", ffi.offsetof("user_t", "score"), 8,  4}

local users = _lib.db_table_register(db, "users", ffi.sizeof("user_t"), fields, 5)

-- Insert
local user1 = ffi.new("user_t")
user1.id = 1; user1.age = 25; user1.score = 95.5
cstring_copy(user1.name, "Alice")
cstring_copy(user1.email, "alice@example.com")
local rowid1 = _lib.db_insert(users, user1, ffi.sizeof("user_t"))
print("Inserted rowid:", tonumber(rowid1))

-- Batch insert
local batch = ffi.new("user_t[3]")
for i = 0, 2 do
    batch[i].id = 2 + i
    batch[i].age = 20 + i * 5
    batch[i].score = 80.0 + i * 5
    ffi.copy(batch[i].name, "User" .. tostring(i), 5)
    batch[i].name[5] = 0
end
local inserted = _lib.db_batch_insert(users, batch, ffi.sizeof("user_t"), 3)
print("Batch inserted:", tonumber(inserted))

-- Select all
local json = _lib.db_select_all_json(users)
print("All users:", ffi.string(json))
_lib.db_json_free(json)

-- Select by PK
json = _lib.db_select_by_pk_json(users, 1)
print("User by PK:", ffi.string(json))
_lib.db_json_free(json)

-- Where
local cond = ffi.new("db_condition_t[1]")
cond[0].field_offset = ffi.offsetof("user_t", "age")
cond[0].field_size = 4
local age_val = ffi.new("int[1]", 25)
cond[0].value = age_val
cond[0].op = 1
json = _lib.db_select_where_json(users, cond, 1)
print("Age > 25:", ffi.string(json))
_lib.db_json_free(json)

-- Update
local update_user = ffi.new("user_t")
update_user.id = 1; update_user.age = 26; update_user.score = 97.0
cstring_copy(update_user.name, "Alice Updated")
cstring_copy(update_user.email, "alice.new@example.com")
local ret = _lib.db_update(users, 1, update_user, ffi.sizeof("user_t"))
print("Update result:", ret)

json = _lib.db_select_by_pk_json(users, 1)
print("After update:", ffi.string(json))
_lib.db_json_free(json)

-- Delete
ret = _lib.db_delete(users, 4)
print("Delete result:", ret)

json = _lib.db_select_all_json(users)
print("After delete:", ffi.string(json))
_lib.db_json_free(json)

-- Count
print("Row count:", tonumber(_lib.db_table_count(users)))

-- Compact
local freed = _lib.db_table_compact(users)
print("Compact freed:", tonumber(freed))

-- Transaction
print("\n--- Transaction ---")

ret = _lib.db_begin(db)
print("BEGIN:", ret)

local tx_user = ffi.new("user_t")
tx_user.id = 99; tx_user.age = 40; tx_user.score = 100.0
cstring_copy(tx_user.name, "TX User")
cstring_copy(tx_user.email, "tx@test.com")

local tx_rowid = _lib.db_insert(users, tx_user, ffi.sizeof("user_t"))
print("TX insert rowid:", tonumber(tx_rowid))

json = _lib.db_select_all_json(users)
print("In TX:", ffi.string(json))
_lib.db_json_free(json)

ret = _lib.db_rollback(db)
print("ROLLBACK:", ret)

json = _lib.db_select_all_json(users)
print("After rollback:", ffi.string(json))
_lib.db_json_free(json)

-- Commit example
ret = _lib.db_begin(db)
print("BEGIN again:", ret)

tx_user.id = 100
local tx_rowid2 = _lib.db_insert(users, tx_user, ffi.sizeof("user_t"))
print("TX insert rowid:", tonumber(tx_rowid2))

ret = _lib.db_commit(db)
print("COMMIT:", ret)

json = _lib.db_select_all_json(users)
print("After commit:", ffi.string(json))
_lib.db_json_free(json)

_lib.db_sync(db)
_lib.db_close(db)
print("\nDone")
