#!/usr/bin/env python3
"""使用 mydb.py 封装层的动态 Schema 示例"""

import sys
sys.path.insert(0, '.')

from mydb import create

print("=== mydb.py 动态封装示例 ===\n")

# 创建数据库并同时注册表（支持 with 语句自动关闭）
with create("dyn_py.bin", schemas={
    "users": [
        {"name": "id",    "type": "uint64"},
        {"name": "name",  "type": "string", "size": 32},
        {"name": "age",   "type": "int32"},
        {"name": "score", "type": "double"},
    ]
}) as db:
    print("1. 数据库创建并注册 users 表成功")
    
    users = db.table("users")
    
    # 创建索引
    users.create_index("name")
    print("2. 创建 name 索引成功")
    
    # 插入数据（用 Python dict，自动序列化）
    id1 = users.insert({"name": "Alice", "age": 25, "score": 95.5})
    id2 = users.insert({"name": "Bob",   "age": 30, "score": 88.0})
    id3 = users.insert({"name": "Carol", "age": 28, "score": 92.0})
    print(f"3. 插入数据: Alice(id={id1}), Bob(id={id2}), Carol(id={id3})")
    
    # 主键查询
    row = users.find(id1)
    print(f"4. 主键查询 id={id1}: {row}")
    
    # 全表查询
    all_rows = users.select()
    print(f"5. 全表查询，共 {len(all_rows)} 行:")
    for r in all_rows:
        print(f"   {r['name']}, age={r['age']}, score={r['score']}")
    
    # WHERE 查询（链式条件）
    adults = users.where(age__gt=25)
    print("6. WHERE age>25:")
    for r in adults:
        print(f"   {r['name']}")
    
    # 更新
    users.update(id1, {"name": "Alice Updated", "age": 26, "score": 96.0})
    print("7. 更新 Alice 成功")
    
    # 删除
    users.delete(id3)
    print("8. 删除 Carol 成功")
    
    # 验证
    print(f"9. 当前行数: {users.count()}")
    
    # Checkpoint
    db.checkpoint()
    print("10. Checkpoint 完成")

print("11. 数据库自动关闭")

print("\n=== 示例完成 ===")
