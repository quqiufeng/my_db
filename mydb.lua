-- mydb.lua - LuaJIT FFI 高级封装
-- 让 my_db 像原生 Lua 库一样使用

local ffi = require("ffi")
local C = ffi.C

-- 加载动态库（搜索常见路径）
local lib_paths = {
    "./libmydb.so",
    "libmydb.so",
    "/usr/local/lib/libmydb.so",
    "/usr/lib/libmydb.so",
}

local _lib = nil
for _, path in ipairs(lib_paths) do
    local ok, lib = pcall(ffi.load, path)
    if ok then
        _lib = lib
        break
    end
end

if not _lib then
    error("Cannot load libmydb.so, tried: " .. table.concat(lib_paths, ", "))
end

-- FFI 声明
ffi.cdef[[
    typedef struct db_instance* db_t;
    typedef struct db_table*    table_t;
    typedef uint64_t            rowid_t;
    
    enum {
        DB_OK = 0, DB_ERR_NOMEM = -1, DB_ERR_IO = -2,
        DB_ERR_NOENT = -3, DB_ERR_EXIST = -4, DB_ERR_INVAL = -5,
        DB_ERR_RESULT_TOO_LARGE = -6, DB_ERR_CORRUPTED = -7,
        DB_ERR_WAL_REPLAY = -8
    };
    
    enum db_field_type {
        DB_TYPE_INT32, DB_TYPE_INT64, DB_TYPE_UINT64,
        DB_TYPE_FLOAT, DB_TYPE_DOUBLE, DB_TYPE_STRING, DB_TYPE_BOOL
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
    
    db_t db_open(const char* dp, const char* ip, const char* wp, size_t ps);
    void db_close(db_t db);
    int db_sync(db_t db);
    int db_wal_replay(db_t db);
    int db_checkpoint(db_t db);
    void db_config_max_rows(db_t db, size_t max);
    
    table_t db_table_register(db_t db, const char* name, size_t rs,
                               const db_field_def_t* fields, size_t fc);
    table_t db_table(db_t db, const char* name);
    int db_table_add_index(table_t t, const char* fn, size_t fo, int ft);
    int db_table_add_index_composite(table_t t, db_field_def_t* fields, size_t fc);
    size_t db_table_count(table_t t);
    
    rowid_t db_insert(table_t t, const void* row, size_t rs);
    int db_update(table_t t, rowid_t id, const void* row, size_t rs);
    int db_delete(table_t t, rowid_t id);
    
    const char* db_select_by_pk_json(table_t t, rowid_t id);
    const char* db_select_all_json(table_t t);
    const char* db_select_where_json(table_t t, const db_condition_t* conds, size_t nc);
    const char* db_select_json(table_t t, const db_condition_t* conds, size_t nc,
                                size_t order_off, int asc, size_t lim_off, size_t lim_cnt);
    const char* db_join_json(table_t lt, size_t lfo, table_t rt, size_t rfo, size_t fs);
    void db_json_free(const char* json);
]]

-- 字段类型映射
local TYPE_MAP = {
    int32 = ffi.C.DB_TYPE_INT32, int64 = ffi.C.DB_TYPE_INT64,
    uint64 = ffi.C.DB_TYPE_UINT64, float = ffi.C.DB_TYPE_FLOAT,
    double = ffi.C.DB_TYPE_DOUBLE, string = ffi.C.DB_TYPE_STRING,
    bool = ffi.C.DB_TYPE_BOOL,
}

local TYPE_SIZE = {
    int32 = 4, int64 = 8, uint64 = 8, float = 4,
    double = 8, string = nil, bool = 1,
}

-- JSON 解析（简单版，足够处理 my_db 的 JSON 输出）
local function parse_json(json_str)
    if json_str == "[]" then return {} end
    if json_str:sub(1,1) ~= "[" then
        -- 单行对象
        local obj = {}
        for k, v in json_str:gmatch('"([^"]+)":([^,{}]+)') do
            v = v:match("^%s*(.-)%s*$")
            if v:sub(1,1) == '"' then
                obj[k] = v:sub(2, -2)
            elseif v == "true" then
                obj[k] = true
            elseif v == "false" then
                obj[k] = false
            elseif v:find("%.") then
                obj[k] = tonumber(v)
            else
                obj[k] = tonumber(v)
            end
        end
        return obj
    end
    
    -- 数组
    local arr = {}
    for obj_str in json_str:gmatch("%b{}") do
        table.insert(arr, parse_json(obj_str))
    end
    return arr
end

-- 计算字段布局（自动对齐到 8 字节）
local function compute_layout(schema)
    local fields = {}
    local offset = 0
    
    -- 第一字段必须是 id（自动添加）
    if not schema[1] or schema[1].name ~= "id" then
        table.insert(fields, 1, {name = "id", type = "uint64", size = 8, offset = 0})
        offset = 8
    end
    
    for _, f in ipairs(schema) do
        local size = f.size or TYPE_SIZE[f.type] or 8
        -- 对齐到 8 字节
        local align = math.min(size, 8)
        if offset % align ~= 0 then
            offset = offset + (align - offset % align)
        end
        
        table.insert(fields, {
            name = f.name,
            type = f.type,
            size = size,
            offset = offset,
        })
        offset = offset + size
    end
    
    -- 最终对齐
    if offset % 8 ~= 0 then
        offset = offset + (8 - offset % 8)
    end
    
    return fields, offset
end

-- 序列化 Lua 表到 C 内存
local function serialize_row(row, layout)
    local buf = ffi.new("uint8_t[?]", layout.row_size)
    ffi.fill(buf, layout.row_size, 0)
    
    for _, f in ipairs(layout.fields) do
        local val = row[f.name]
        if val ~= nil then
            local ptr = buf + f.offset
            if f.type == "uint64" then
                ffi.cast("uint64_t*", ptr)[0] = val
            elseif f.type == "int32" then
                ffi.cast("int32_t*", ptr)[0] = val
            elseif f.type == "int64" then
                ffi.cast("int64_t*", ptr)[0] = val
            elseif f.type == "float" then
                ffi.cast("float*", ptr)[0] = val
            elseif f.type == "double" then
                ffi.cast("double*", ptr)[0] = val
            elseif f.type == "bool" then
                ptr[0] = val and 1 or 0
            elseif f.type == "string" then
                local s = tostring(val)
                local len = math.min(#s, f.size - 1)
                ffi.copy(ptr, s, len)
            end
        end
    end
    
    return buf
end

-- Table 对象
local Table = {}
Table.__index = Table

function Table:insert(row)
    local buf = serialize_row(row, self._layout)
    local id = _lib.db_insert(self._ptr, buf, self._layout.row_size)
    return tonumber(id)
end

function Table:find(id)
    local json = _lib.db_select_by_pk_json(self._ptr, id)
    if json == nil then return nil end
    local str = ffi.string(json)
    _lib.db_json_free(json)
    return parse_json(str)
end

function Table:select()
    local json = _lib.db_select_all_json(self._ptr)
    if json == nil then return {} end
    local str = ffi.string(json)
    _lib.db_json_free(json)
    return parse_json(str)
end

function Table:where(conditions)
    if not conditions or #conditions == 0 then
        return self:select()
    end
    
    local conds = ffi.new("db_condition_t[?]", #conditions)
    local vals = {} -- 保持值存活
    
    for i, c in ipairs(conditions) do
        local field = self._layout.field_map[c.field]
        if not field then
            error("Unknown field: " .. tostring(c.field))
        end
        
        local val = c.value
        local val_ptr
        if field.type == "int32" then
            local v = ffi.new("int32_t[1]", val)
            val_ptr = v
            table.insert(vals, v)
        elseif field.type == "uint64" then
            local v = ffi.new("uint64_t[1]", val)
            val_ptr = v
            table.insert(vals, v)
        elseif field.type == "double" then
            local v = ffi.new("double[1]", val)
            val_ptr = v
            table.insert(vals, v)
        elseif field.type == "string" then
            local s = tostring(val)
            val_ptr = s
            table.insert(vals, s)
        end
        
        conds[i-1].field_offset = field.offset
        conds[i-1].field_size = field.size
        conds[i-1].value = val_ptr
        conds[i-1].op = c.op or 0  -- 0=等于, 1=大于, 2=小于
    end
    
    local json = _lib.db_select_where_json(self._ptr, conds, #conditions)
    if json == nil then return {} end
    local str = ffi.string(json)
    _lib.db_json_free(json)
    return parse_json(str)
end

function Table:update(id, row)
    local buf = serialize_row(row, self._layout)
    return _lib.db_update(self._ptr, id, buf, self._layout.row_size)
end

function Table:delete(id)
    return _lib.db_delete(self._ptr, id)
end

function Table:count()
    return tonumber(_lib.db_table_count(self._ptr))
end

function Table:create_index(field_name)
    local field = self._layout.field_map[field_name]
    if not field then
        error("Unknown field: " .. tostring(field_name))
    end
    return _lib.db_table_add_index(self._ptr, field_name, field.offset, TYPE_MAP[field.type])
end

-- DB 对象
local DB = {}
DB.__index = DB

function DB:register(name, schema)
    local fields, row_size = compute_layout(schema)
    
    local cfields = ffi.new("db_field_def_t[?]", #fields)
    for i, f in ipairs(fields) do
        cfields[i-1].name = f.name
        cfields[i-1].offset = f.offset
        cfields[i-1].size = f.size
        cfields[i-1].type = TYPE_MAP[f.type]
    end
    
    local ptr = _lib.db_table_register(self._ptr, name, row_size, cfields, #fields)
    if ptr == nil then
        error("Failed to register table: " .. name)
    end
    
    -- 构建字段映射
    local field_map = {}
    for _, f in ipairs(fields) do
        field_map[f.name] = f
    end
    
    local layout = {
        fields = fields,
        field_map = field_map,
        row_size = row_size,
    }
    
    self._tables[name] = layout
    
    local table_obj = setmetatable({
        _ptr = ptr,
        _layout = layout,
        _name = name,
    }, Table)
    
    return table_obj
end

function DB:table(name)
    local ptr = _lib.db_table(self._ptr, name)
    if ptr == nil then
        error("Table not found: " .. name)
    end
    
    local layout = self._tables[name]
    if not layout then
        error("Table not registered in this session: " .. name)
    end
    
    return setmetatable({
        _ptr = ptr,
        _layout = layout,
        _name = name,
    }, Table)
end

function DB:checkpoint()
    return _lib.db_checkpoint(self._ptr)
end

function DB:close()
    if self._ptr then
        _lib.db_close(self._ptr)
        self._ptr = nil
    end
end

function DB:max_rows(n)
    _lib.db_config_max_rows(self._ptr, n)
end

-- 模块 API
local mydb = {}

function mydb.open(data_path, index_path, wal_path, pool_size)
    data_path = data_path or "mydb_data.bin"
    index_path = index_path or "mydb_index.index"
    wal_path = wal_path or "mydb_wal.bin"
    pool_size = pool_size or (1024 * 1024 * 100)
    
    local ptr = _lib.db_open(data_path, index_path, wal_path, pool_size)
    if ptr == nil then
        error("Failed to open database")
    end
    
    return setmetatable({
        _ptr = ptr,
        _tables = {},
    }, DB)
end

-- 便捷函数：直接打开并注册表
function mydb.create(path, schemas)
    local db = mydb.open(path, path .. ".index", path .. ".wal")
    
    if schemas then
        for name, schema in pairs(schemas) do
            db:register(name, schema)
        end
    end
    
    return db
end

return mydb
