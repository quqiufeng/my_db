#!/usr/bin/env python3
# my_db Python ctypes 示例

import ctypes

lib = ctypes.CDLL("./libmydb.so")

lib.db_open.restype = ctypes.c_void_p
lib.db_table_register.restype = ctypes.c_void_p
lib.db_insert.restype = ctypes.c_uint64
lib.db_select_all_json.restype = ctypes.c_void_p
lib.db_json_free.argtypes = [ctypes.c_void_p]

class Field(ctypes.Structure):
    _fields_ = [("name", ctypes.c_char_p), ("offset", ctypes.c_size_t),
                ("size", ctypes.c_size_t), ("type", ctypes.c_int)]

class User(ctypes.Structure):
    _fields_ = [("id", ctypes.c_uint64), ("name", ctypes.c_char * 32),
                ("age", ctypes.c_int32), ("score", ctypes.c_double)]

print("=== my_db Python ctypes 示例 ===\n")

# 打开数据库
db = lib.db_open(b"ex_py.bin", b"ex_py.idx", b"ex_py.wal", 1024*1024*10)
print("1. 数据库打开成功")

# 注册表
fields = (Field * 4)(
    Field(b"id", 0, 8, 2), Field(b"name", 8, 32, 5),
    Field(b"age", 40, 4, 0), Field(b"score", 48, 8, 4)
)
users = lib.db_table_register(db, b"users", ctypes.sizeof(User), fields, 4)
print("2. Schema 注册成功")

# 插入数据
u = User()
u.id = 0
u.name = b"Alice\x00"
u.age = 25
u.score = 95.5
id1 = lib.db_insert(users, ctypes.byref(u), ctypes.sizeof(User))
print(f"3. 插入 Alice, id={id1}")

# 查询
json_ptr = lib.db_select_all_json(users)
if json_ptr:
    s = ctypes.cast(json_ptr, ctypes.c_char_p).value.decode('utf-8')
    print(f"4. 全表查询: {s}")
    lib.db_json_free(json_ptr)

# Checkpoint
lib.db_checkpoint(db)
print("5. Checkpoint 完成")

# 关闭
lib.db_close(db)
print("6. 数据库关闭")

print("\n=== 示例完成 ===")
