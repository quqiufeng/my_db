-- mydb.lua - LuaJIT FFI 高级封装
-- 让 my_db 像原生 Lua 库一样使用

local ffi = require("ffi")
local C = ffi.C

-- 加载动态库（搜索常见路径）
local lib_paths = {
    "./libmydb.so",
    "libmydb.so",
    "/opt/my_db/libmydb.so",
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
    
    db_t db_open(const char* db_dir, size_t pool_size);
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

function Table:create_index_composite(field_names)
    if type(field_names) ~= "table" or #field_names == 0 then
        error("Composite index requires a non-empty array of field names")
    end
    if #field_names > 4 then
        error("Composite index supports up to 4 fields")
    end
    
    local cfields = ffi.new("db_field_def_t[?]", #field_names)
    for i, fname in ipairs(field_names) do
        local field = self._layout.field_map[fname]
        if not field then
            error("Unknown field: " .. tostring(fname))
        end
        cfields[i-1].name = fname
        cfields[i-1].offset = field.offset
        cfields[i-1].size = field.size
        cfields[i-1].type = TYPE_MAP[field.type]
    end
    
    return _lib.db_table_add_index_composite(self._ptr, cfields, #field_names)
end

-- DB 对象
local DB = {}
DB.__index = DB

function DB:register(name, schema, indexes)
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
    
    -- 自动创建索引（如果指定了）
    if indexes then
        for _, idx in ipairs(indexes) do
            if type(idx) == "string" then
                -- 单列索引
                table_obj:create_index(idx)
            elseif type(idx) == "table" then
                -- 复合索引
                table_obj:create_index_composite(idx)
            end
        end
    end
    
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

function DB:sync()
    return _lib.db_sync(self._ptr)
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

-- ========================================================================
-- KV Cache FFI 声明
-- ========================================================================

ffi.cdef[[
    typedef struct cache cache_t;
    typedef struct cache_iter cache_iter_t;
    
    typedef struct {
        const char* key;
        const char* value;
        double score;
    } cache_result_t;
    
    typedef struct {
        int max_results;
        int case_sensitive;
        const char* ns_filter;
    } cache_search_options_t;
    
    cache_t* cache_open(const char* db_dir, size_t max_memory);
    void cache_close(cache_t* cache);
    int cache_sync(cache_t* cache);
    
    int cache_set(cache_t* cache, const char* key, const char* value, uint64_t ttl_ms);
    const char* cache_get(cache_t* cache, const char* key);
    int cache_del(cache_t* cache, const char* key);
    int cache_exists(cache_t* cache, const char* key);
    
    size_t cache_count(cache_t* cache);
    size_t cache_memory_used(cache_t* cache);
    size_t cache_memory_max(cache_t* cache);
    
    int cache_search_prefix(cache_t* cache, const char* prefix,
                            const cache_search_options_t* options,
                            cache_result_t** out_results, size_t* out_count);
    int cache_search_range(cache_t* cache, const char* start_key, const char* end_key,
                           const cache_search_options_t* options,
                           cache_result_t** out_results, size_t* out_count);
    int cache_search_regex(cache_t* cache, const char* pattern,
                           const cache_search_options_t* options,
                           cache_result_t** out_results, size_t* out_count);
    int cache_search_fuzzy(cache_t* cache, const char* query,
                           const cache_search_options_t* options,
                           cache_result_t** out_results, size_t* out_count);
    int cache_search_tag(cache_t* cache, const char* tag,
                         const cache_search_options_t* options,
                         cache_result_t** out_results, size_t* out_count);
    void cache_results_free(cache_result_t* results);
    
    cache_iter_t* cache_iter_create(cache_t* cache);
    void cache_iter_destroy(cache_iter_t* iter);
    int cache_iter_next(cache_iter_t* iter, const char** key_out, const char** value_out);
    void cache_iter_reset(cache_iter_t* iter);
    
    int cache_set_ns(cache_t* cache, const char* ns, const char* key, 
                     const char* value, uint64_t ttl_ms);
    const char* cache_get_ns(cache_t* cache, const char* ns, const char* key);
    int cache_del_ns(cache_t* cache, const char* ns, const char* key);
    
    int cache_expire(cache_t* cache, const char* key);
    int cache_touch(cache_t* cache, const char* key);
    size_t cache_compact(cache_t* cache);
    size_t cache_purge_expired(cache_t* cache);
    int cache_check(const char* db_dir);
]]

-- ========================================================================
-- Cache 对象
-- ========================================================================

local Cache = {}
Cache.__index = Cache

function Cache:set(key, value, ttl_ms)
    ttl_ms = ttl_ms or 0
    local ret = _lib.cache_set(self._ptr, key, value, ttl_ms)
    if ret ~= 0 then
        error("cache_set failed: " .. ret)
    end
    return self
end

function Cache:get(key)
    local ptr = _lib.cache_get(self._ptr, key)
    if ptr == nil then return nil end
    return ffi.string(ptr)
end

function Cache:delete(key)
    local ret = _lib.cache_del(self._ptr, key)
    if ret == -4 then
        error("Key not found: " .. key)
    end
    return ret == 0
end

function Cache:exists(key)
    return _lib.cache_exists(self._ptr, key) ~= 0
end

function Cache:set_json(key, obj, ttl_ms)
    local json = require("cjson").encode(obj)
    return self:set(key, json, ttl_ms)
end

function Cache:get_json(key)
    local value = self:get(key)
    if not value then return nil end
    return require("cjson").decode(value)
end

-- Namespace
function Cache:set_ns(ns, key, value, ttl_ms)
    ttl_ms = ttl_ms or 0
    local ret = _lib.cache_set_ns(self._ptr, ns, key, value, ttl_ms)
    if ret ~= 0 then
        error("cache_set_ns failed: " .. ret)
    end
end

function Cache:get_ns(ns, key)
    local ptr = _lib.cache_get_ns(self._ptr, ns, key)
    if ptr == nil then return nil end
    return ffi.string(ptr)
end

function Cache:delete_ns(ns, key)
    local ret = _lib.cache_del_ns(self._ptr, ns, key)
    if ret == -4 then
        error("Key not found: " .. ns .. "/" .. key)
    end
end

-- Search helpers
local function search_helper(cache, func_name, query, max_results)
    max_results = max_results or 100
    local opts = ffi.new("cache_search_options_t")
    opts.max_results = max_results
    opts.case_sensitive = 0
    opts.ns_filter = nil
    
    local results_ptr = ffi.new("cache_result_t*[1]")
    local count = ffi.new("size_t[1]")
    
    local func = _lib[func_name]
    local ret = func(cache._ptr, query, opts, results_ptr, count)
    
    if ret ~= 0 then
        return {}
    end
    
    local results = {}
    local n = tonumber(count[0])
    for i = 0, n - 1 do
        local r = results_ptr[0][i]
        table.insert(results, {
            key = ffi.string(r.key),
            value = ffi.string(r.value),
            score = tonumber(r.score),
        })
    end
    
    if results_ptr[0] ~= nil then
        _lib.cache_results_free(results_ptr[0])
    end
    
    return results
end

function Cache:search_prefix(prefix, max_results)
    return search_helper(self, "cache_search_prefix", prefix, max_results)
end

function Cache:search_regex(pattern, max_results)
    return search_helper(self, "cache_search_regex", pattern, max_results)
end

function Cache:search_fuzzy(query, max_results)
    return search_helper(self, "cache_search_fuzzy", query, max_results)
end

function Cache:search_tag(tag, max_results)
    return search_helper(self, "cache_search_tag", tag, max_results)
end

-- Iterator
function Cache:items()
    local iter = _lib.cache_iter_create(self._ptr)
    if iter == nil then return function() end end
    
    return function()
        local key_ptr = ffi.new("const char*[1]")
        local value_ptr = ffi.new("const char*[1]")
        
        if _lib.cache_iter_next(iter, key_ptr, value_ptr) == 1 then
            return ffi.string(key_ptr[0]), ffi.string(value_ptr[0])
        else
            _lib.cache_iter_destroy(iter)
            return nil
        end
    end
end

function Cache:keys()
    return coroutine.wrap(function()
        for k, _ in self:items() do
            coroutine.yield(k)
        end
    end)
end

function Cache:values()
    return coroutine.wrap(function()
        for _, v in self:items() do
            coroutine.yield(v)
        end
    end)
end

-- Management
function Cache:expire(key)
    local ret = _lib.cache_expire(self._ptr, key)
    if ret == -4 then error("Key not found: " .. key) end
end

function Cache:touch(key)
    local ret = _lib.cache_touch(self._ptr, key)
    if ret == -4 then error("Key not found: " .. key) end
end

function Cache:compact()
    return tonumber(_lib.cache_compact(self._ptr))
end

function Cache:purge_expired()
    return tonumber(_lib.cache_purge_expired(self._ptr))
end

function Cache:sync()
    return _lib.cache_sync(self._ptr)
end

function Cache:close()
    if self._ptr then
        _lib.cache_close(self._ptr)
        self._ptr = nil
    end
end

function Cache:__tostring()
    return string.format("Cache(count=%d, memory=%d/%d)",
        self:count(), self:memory_used(), self:memory_max())
end

-- Properties
function Cache:count()
    return tonumber(_lib.cache_count(self._ptr))
end

function Cache:memory_used()
    return tonumber(_lib.cache_memory_used(self._ptr))
end

function Cache:memory_max()
    return tonumber(_lib.cache_memory_max(self._ptr))
end

-- ========================================================================
-- 模块 API
-- ========================================================================

local mydb = {}

function mydb.open(db_dir, pool_size)
    db_dir = db_dir or "mydb_data"
    pool_size = pool_size or (1024 * 1024 * 100)
    
    local ptr = _lib.db_open(db_dir, pool_size)
    if ptr == nil then
        error("Failed to open database directory: " .. db_dir)
    end
    
    return setmetatable({
        _ptr = ptr,
        _tables = {},
    }, DB)
end

-- 便捷函数：直接打开并注册表
function mydb.create(db_dir, schemas)
    local db = mydb.open(db_dir)
    
    if schemas then
        for name, schema in pairs(schemas) do
            db:register(name, schema)
        end
    end
    
    return db
end

-- KV Cache
function mydb.open_cache(db_dir, max_memory)
    db_dir = db_dir or "cache_data"
    max_memory = max_memory or (100 * 1024 * 1024)
    
    local ptr = _lib.cache_open(db_dir, max_memory)
    if ptr == nil then
        error("Failed to open cache: " .. db_dir)
    end
    
    return setmetatable({
        _ptr = ptr,
    }, Cache)
end

function mydb.cache_check(db_dir)
    return _lib.cache_check(db_dir) == 0
end

function Cache:search_semantic(cache_dir, query, max_results, namespace)
    max_results = max_results or 10
    local cmd = string.format("/opt/my_db/tools/vector_search --json %s %q %d",
        cache_dir, query, max_results)
    if namespace and namespace ~= "" then
        cmd = cmd .. " " .. namespace
    end
    local f = io.popen(cmd, "r")
    if not f then return {} end
    local output = f:read("*a")
    f:close()
    if not output or output == "" then return {} end
    local ok, results = pcall(require("cjson").decode, output)
    if ok and type(results) == "table" then return results end
    return {}
end

return mydb
