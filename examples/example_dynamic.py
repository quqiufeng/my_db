#!/usr/bin/env python3
"""使用 mydb.py 封装层的动态 Schema 示例"""

import sys
sys.path.insert(0, '.')

from mydb import create

print("=== mydb.py 多文件存储示例 ===\n")

# 创建数据库目录并注册表（支持 with 语句自动关闭）
with create("game_db", schemas={
    "users": [
        {"name": "id",    "type": "uint64"},
        {"name": "name",  "type": "string", "size": 32},
        {"name": "age",   "type": "int32"},
        {"name": "score", "type": "double"},
    ],
    "orders": [
        {"name": "id",      "type": "uint64"},
        {"name": "user_id", "type": "uint64"},
        {"name": "amount",  "type": "double"},
    ]
}) as db:
    print("1. 数据库目录创建成功")
    print("   目录结构: game_db/")
    print("   ├── wal.bin        (WAL日志)")
    print("   ├── users.bin      (users表数据)")
    print("   ├── users.index    (users表索引)")
    print("   ├── orders.bin     (orders表数据)")
    print("   └── orders.index   (orders表索引)")
    
    users = db.table("users")
    orders = db.table("orders")
    
    # 创建索引
    users.create_index("name")
    orders.create_index("user_id")
    print("2. 索引创建成功")
    
    # 插入数据
    id1 = users.insert({"name": "Alice", "age": 25, "score": 95.5})
    id2 = users.insert({"name": "Bob",   "age": 30, "score": 88.0})
    id3 = users.insert({"name": "Carol", "age": 28, "score": 92.0})
    print(f"3. 插入数据: Alice(id={id1}), Bob(id={id2}), Carol(id={id3})")
    
    # 插入订单
    orders.insert({"user_id": id1, "amount": 100.0})
    orders.insert({"user_id": id1, "amount": 250.0})
    orders.insert({"user_id": id2, "amount": 50.0})
    print("4. 插入订单: Alice 2 笔, Bob 1 笔")
    
    # 主键查询
    row = users.find(id1)
    print(f"5. 主键查询 id={id1}: {row}")
    
    # 全表查询
    all_rows = users.select()
    print(f"6. 全表查询，共 {len(all_rows)} 行:")
    for r in all_rows:
        print(f"   {r['name']}, age={r['age']}, score={r['score']}")
    
    # WHERE 查询
    adults = users.where(age__gt=25)
    print("7. WHERE age>25:")
    for r in adults:
        print(f"   {r['name']}")
    
    # 更新
    users.update(id1, {"name": "Alice Updated", "age": 26, "score": 96.0})
    print("8. 更新 Alice 成功")
    
    # 删除
    users.delete(id3)
    print("9. 删除 Carol 成功")
    
    # 验证
    print(f"10. 当前行数: {users.count()}")
    
    # 强制刷盘
    db.sync()
    print("11. 数据已强制刷盘到各自文件")
    
    # Checkpoint
    db.checkpoint()
    print("12. Checkpoint 完成")

print("13. 数据库自动关闭")

print("\n=== 示例完成 ===")

import os
print("\n文件结构:")
for f in sorted(os.listdir("game_db")):
    path = os.path.join("game_db", f)
    size = os.path.getsize(path)
    print(f"   {f:20s} {size:10d} bytes")
