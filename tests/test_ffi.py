#!/usr/bin/env python3
# my_db Python ctypes 测试

import ctypes
import os
import sys

# 加载动态库
lib = ctypes.CDLL("./libmydb.so")

# 定义类型
rowid_t = ctypes.c_uint64

# 定义枚举
(DB_OK, DB_ERR_NOMEM, DB_ERR_IO, DB_ERR_NOENT, DB_ERR_EXIST,
 DB_ERR_INVAL, DB_ERR_RESULT_TOO_LARGE, DB_ERR_CORRUPTED,
 DB_ERR_WAL_REPLAY) = range(0, -9, -1)

(DB_TYPE_INT32, DB_TYPE_INT64, DB_TYPE_UINT64, DB_TYPE_FLOAT,
 DB_TYPE_DOUBLE, DB_TYPE_STRING, DB_TYPE_BOOL) = range(7)

# 定义结构
class db_field_def_t(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char_p),
        ("offset", ctypes.c_size_t),
        ("size", ctypes.c_size_t),
        ("type", ctypes.c_int),
    ]

class db_condition_t(ctypes.Structure):
    _fields_ = [
        ("field_offset", ctypes.c_size_t),
        ("field_size", ctypes.c_size_t),
        ("value", ctypes.c_void_p),
        ("op", ctypes.c_int),
    ]

class ffi_user(ctypes.Structure):
    _fields_ = [
        ("id", ctypes.c_uint64),
        ("name", ctypes.c_char * 32),
        ("age", ctypes.c_int32),
        ("score", ctypes.c_double),
    ]

# 配置函数签名
lib.db_open.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t]
lib.db_open.restype = ctypes.c_void_p

lib.db_close.argtypes = [ctypes.c_void_p]
lib.db_close.restype = None

lib.db_table_register.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t,
                                  ctypes.POINTER(db_field_def_t), ctypes.c_size_t]
lib.db_table_register.restype = ctypes.c_void_p

lib.db_table.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
lib.db_table.restype = ctypes.c_void_p

lib.db_table_count.argtypes = [ctypes.c_void_p]
lib.db_table_count.restype = ctypes.c_size_t

lib.db_insert.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
lib.db_insert.restype = rowid_t

lib.db_update.argtypes = [ctypes.c_void_p, rowid_t, ctypes.c_void_p, ctypes.c_size_t]
lib.db_update.restype = ctypes.c_int

lib.db_delete.argtypes = [ctypes.c_void_p, rowid_t]
lib.db_delete.restype = ctypes.c_int

lib.db_select_by_pk_json.argtypes = [ctypes.c_void_p, rowid_t]
lib.db_select_by_pk_json.restype = ctypes.c_void_p

lib.db_select_all_json.argtypes = [ctypes.c_void_p]
lib.db_select_all_json.restype = ctypes.c_void_p

lib.db_select_where_json.argtypes = [ctypes.c_void_p,
                                      ctypes.POINTER(db_condition_t), ctypes.c_size_t]
lib.db_select_where_json.restype = ctypes.c_void_p

lib.db_json_free.argtypes = [ctypes.c_void_p]
lib.db_json_free.restype = None

test_count = 0
pass_count = 0

def check(cond, msg):
    global test_count, pass_count
    test_count += 1
    if cond:
        pass_count += 1
        print(f"[OK] {msg}")
    else:
        print(f"[FAIL] {msg}")

print("=== my_db Python ctypes 测试 ===\n")

# 清理旧文件
for f in ["py_data.bin", "py_index.index", "py_wal.bin"]:
    if os.path.exists(f):
        os.remove(f)

# 打开数据库
db = lib.db_open(b"py_data.bin", b"py_index.index", b"py_wal.bin", 1024*1024*10)
check(db is not None, "数据库打开成功")

# 注册表结构
fields = (db_field_def_t * 4)(
    db_field_def_t(b"id", 0, 8, DB_TYPE_UINT64),
    db_field_def_t(b"name", 8, 32, DB_TYPE_STRING),
    db_field_def_t(b"age", 40, 4, DB_TYPE_INT32),
    db_field_def_t(b"score", 48, 8, DB_TYPE_DOUBLE),
)

users = lib.db_table_register(db, b"py_users", ctypes.sizeof(ffi_user), fields, 4)
check(users is not None, "Schema 注册成功")

# 插入数据
u1 = ffi_user()
u1.id = 0
u1.name = b"Alice\x00"
u1.age = 25
u1.score = 95.5

id1 = lib.db_insert(users, ctypes.byref(u1), ctypes.sizeof(ffi_user))
check(id1 == 1, "插入 Alice, id=1")

u2 = ffi_user()
u2.id = 0
u2.name = b"Bob\x00"
u2.age = 30
u2.score = 88.0

id2 = lib.db_insert(users, ctypes.byref(u2), ctypes.sizeof(ffi_user))
check(id2 == 2, "插入 Bob, id=2")

def ptr_to_str(ptr):
    if not ptr:
        return None
    return ctypes.cast(ptr, ctypes.c_char_p).value.decode('utf-8')

# 查询单条
json1 = lib.db_select_by_pk_json(users, id1)
check(json1 is not None, "主键查询成功")
if json1:
    s = ptr_to_str(json1)
    check("Alice" in s, "查询结果包含 'Alice'")
    lib.db_json_free(json1)

# 全表查询
all_json = lib.db_select_all_json(users)
check(all_json is not None, "全表查询成功")
if all_json:
    s = ptr_to_str(all_json)
    check("Alice" in s, "全表结果包含 Alice")
    check("Bob" in s, "全表结果包含 Bob")
    lib.db_json_free(all_json)

# WHERE 查询
age_val = ctypes.c_int32(20)
cond = db_condition_t()
cond.field_offset = 40
cond.field_size = 4
cond.value = ctypes.addressof(age_val)
cond.op = 1  # >

where_json = lib.db_select_where_json(users, ctypes.byref(cond), 1)
check(where_json is not None, "WHERE 查询成功")
if where_json:
    s = ptr_to_str(where_json)
    check("Alice" in s, "WHERE 结果包含 Alice (age 25 > 20)")
    check("Bob" in s, "WHERE 结果包含 Bob (age 30 > 20)")
    lib.db_json_free(where_json)

# 更新
u1_new = ffi_user()
u1_new.id = id1
u1_new.name = b"Alice Updated\x00"
u1_new.age = 26
u1_new.score = 96.0

ret = lib.db_update(users, id1, ctypes.byref(u1_new), ctypes.sizeof(ffi_user))
check(ret == 0, "更新 Alice 成功")

updated_json = lib.db_select_by_pk_json(users, id1)
if updated_json:
    s = ptr_to_str(updated_json)
    check("Alice Updated" in s, "更新后查询结果正确")
    lib.db_json_free(updated_json)

# 删除
ret = lib.db_delete(users, id2)
check(ret == 0, "删除 Bob 成功")

count = lib.db_table_count(users)
check(count == 1, "删除后表行数=1")

# 关闭数据库
lib.db_close(db)

print(f"\n=== 测试结果: {pass_count}/{test_count} 通过 ===")

if pass_count != test_count:
    sys.exit(1)
