#!/usr/bin/env python3
"""
mydb.py - Python ctypes 高级封装
让 my_db 像原生 Python 库一样使用
"""

import ctypes
import os
import json
import struct

# 加载动态库
lib_paths = [
    "./libmydb.so",
    "libmydb.so",
    "/usr/local/lib/libmydb.so",
    "/usr/lib/libmydb.so",
]

_lib = None
for path in lib_paths:
    if os.path.exists(path):
        try:
            _lib = ctypes.CDLL(path)
            break
        except OSError:
            continue

if _lib is None:
    raise RuntimeError(f"Cannot load libmydb.so, tried: {lib_paths}")

# 类型定义
rowid_t = ctypes.c_uint64

(DB_TYPE_INT32, DB_TYPE_INT64, DB_TYPE_UINT64, DB_TYPE_FLOAT,
 DB_TYPE_DOUBLE, DB_TYPE_STRING, DB_TYPE_BOOL) = range(7)

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

# 配置函数签名
_lib.db_open.restype = ctypes.c_void_p
_lib.db_close.argtypes = [ctypes.c_void_p]
_lib.db_checkpoint.argtypes = [ctypes.c_void_p]
_lib.db_config_max_rows.argtypes = [ctypes.c_void_p, ctypes.c_size_t]

_lib.db_table_register.restype = ctypes.c_void_p
_lib.db_table.restype = ctypes.c_void_p
_lib.db_table_add_index.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int]
_lib.db_table_count.argtypes = [ctypes.c_void_p]
_lib.db_table_count.restype = ctypes.c_size_t

_lib.db_insert.restype = rowid_t
_lib.db_update.argtypes = [ctypes.c_void_p, rowid_t, ctypes.c_void_p, ctypes.c_size_t]
_lib.db_delete.argtypes = [ctypes.c_void_p, rowid_t]

_lib.db_select_by_pk_json.restype = ctypes.c_void_p
_lib.db_select_all_json.restype = ctypes.c_void_p
_lib.db_select_where_json.restype = ctypes.c_void_p
_lib.db_json_free.argtypes = [ctypes.c_void_p]

# 类型映射
TYPE_MAP = {
    'int32': DB_TYPE_INT32, 'int64': DB_TYPE_INT64,
    'uint64': DB_TYPE_UINT64, 'float': DB_TYPE_FLOAT,
    'double': DB_TYPE_DOUBLE, 'string': DB_TYPE_STRING,
    'bool': DB_TYPE_BOOL,
}

TYPE_SIZE = {
    'int32': 4, 'int64': 8, 'uint64': 8, 'float': 4,
    'double': 8, 'string': None, 'bool': 1,
}


def _parse_json_result(ptr):
    """解析 JSON 查询结果（处理字符串中的 null 字节）"""
    if not ptr:
        return None
    
    # 直接读取原始字节，不通过 c_char_p（它会停在第一个 \0）
    # 先找到长度（通过查找结尾的 } 或 ]）
    raw = ctypes.cast(ptr, ctypes.POINTER(ctypes.c_char))
    max_len = 65536  # 最大 64KB
    length = 0
    for i in range(max_len):
        if raw[i] == b'\0':
            length = i
            break
    
    result = bytes(raw[:length]).decode('utf-8', errors='replace')
    _lib.db_json_free(ptr)
    
    if result == "[]":
        return []
    if not result or result == "null":
        return None
    
    # 手动解析 my_db 的 JSON（简单实现，足够处理其输出格式）
    return _manual_parse_json(result)


def _manual_parse_json(s):
    """手动解析 my_db 生成的简单 JSON（无嵌套对象）"""
    s = s.strip()
    if s == "[]":
        return []
    if s.startswith('[') and s.endswith(']'):
        # 数组：按最外层 {} 分割
        items = []
        depth = 0
        obj_start = -1
        for i, c in enumerate(s):
            if c == '{':
                if depth == 0:
                    obj_start = i
                depth += 1
            elif c == '}':
                depth -= 1
                if depth == 0 and obj_start >= 0:
                    items.append(_manual_parse_json(s[obj_start:i+1]))
                    obj_start = -1
        return items
    elif s.startswith('{') and s.endswith('}'):
        # 对象：按逗号分割键值对（注意字符串值内可能有逗号）
        obj = {}
        content = s[1:-1]
        # 按 "key" 分割
        parts = []
        current = ""
        in_string = False
        for c in content:
            if c == '"' and (not current or current[-1] != '\\'):
                in_string = not in_string
            if c == ',' and not in_string:
                parts.append(current)
                current = ""
            else:
                current += c
        if current:
            parts.append(current)
        
        for part in parts:
            part = part.strip()
            if not part:
                continue
            # 找冒号
            colon_idx = -1
            in_str = False
            for i, c in enumerate(part):
                if c == '"' and (i == 0 or part[i-1] != '\\'):
                    in_str = not in_str
                if c == ':' and not in_str:
                    colon_idx = i
                    break
            
            if colon_idx < 0:
                continue
            
            key_part = part[:colon_idx].strip()
            val_part = part[colon_idx+1:].strip()
            
            # 解析键
            key = key_part.strip('"')
            
            # 解析值
            if val_part.startswith('"') and val_part.endswith('"'):
                val = val_part[1:-1]
            elif val_part == 'true':
                val = True
            elif val_part == 'false':
                val = False
            elif val_part == 'null':
                val = None
            elif '.' in val_part:
                val = float(val_part)
            else:
                val = int(val_part)
            
            obj[key] = val
        return obj
    else:
        return None


def _compute_layout(schema):
    """计算字段布局（自动对齐到 8 字节）"""
    fields = []
    offset = 0
    
    # 第一字段必须是 id
    if not schema or schema[0].get('name') != 'id':
        fields.append({'name': 'id', 'type': 'uint64', 'size': 8, 'offset': 0})
        offset = 8
    
    for f in schema:
        size = f.get('size', TYPE_SIZE.get(f['type'], 8))
        align = min(size, 8)
        if offset % align != 0:
            offset += align - (offset % align)
        
        fields.append({
            'name': f['name'],
            'type': f['type'],
            'size': size,
            'offset': offset,
        })
        offset += size
    
    # 最终对齐
    if offset % 8 != 0:
        offset += 8 - (offset % 8)
    
    return fields, offset


def _serialize_row(row_dict, layout_fields, row_size):
    """序列化字典到字节"""
    buf = bytearray(row_size)
    
    for f in layout_fields:
        val = row_dict.get(f['name'])
        if val is not None:
            off = f['offset']
            if f['type'] == 'uint64':
                struct.pack_into('<Q', buf, off, int(val))
            elif f['type'] == 'int32':
                struct.pack_into('<i', buf, off, int(val))
            elif f['type'] == 'int64':
                struct.pack_into('<q', buf, off, int(val))
            elif f['type'] == 'float':
                struct.pack_into('<f', buf, off, float(val))
            elif f['type'] == 'double':
                struct.pack_into('<d', buf, off, float(val))
            elif f['type'] == 'bool':
                buf[off] = 1 if val else 0
            elif f['type'] == 'string':
                s = str(val).encode('utf-8')
                buf[off:off + min(len(s), f['size'] - 1)] = s[:f['size'] - 1]
    
    return bytes(buf)


class Table:
    """表对象"""
    
    def __init__(self, ptr, layout, name):
        self._ptr = ptr
        self._layout = layout
        self._name = name
    
    def insert(self, row):
        """插入一行，返回 id"""
        buf = _serialize_row(row, self._layout['fields'], self._layout['row_size'])
        buf_ptr = ctypes.create_string_buffer(buf)
        return _lib.db_insert(self._ptr, buf_ptr, self._layout['row_size'])
    
    def find(self, id):
        """主键查询"""
        ptr = _lib.db_select_by_pk_json(self._ptr, id)
        return _parse_json_result(ptr)
    
    def select(self):
        """全表查询"""
        ptr = _lib.db_select_all_json(self._ptr)
        return _parse_json_result(ptr)
    
    def where(self, **kwargs):
        """WHERE 查询
        
        用法:
            table.where(age=25)           # age = 25
            table.where(age__gt=25)       # age > 25
            table.where(age__lt=25)       # age < 25
        """
        conditions = []
        value_holders = []  # 保持值存活
        
        for key, val in kwargs.items():
            op = 0  # 等于
            field_name = key
            
            if key.endswith('__gt'):
                op = 1
                field_name = key[:-4]
            elif key.endswith('__lt'):
                op = 2
                field_name = key[:-4]
            
            field = self._layout['field_map'].get(field_name)
            if not field:
                raise ValueError(f"Unknown field: {field_name}")
            
            # 创建值指针
            if field['type'] == 'int32':
                v = ctypes.c_int32(int(val))
                value_holders.append(v)
                val_ptr = ctypes.addressof(v)
            elif field['type'] == 'uint64':
                v = ctypes.c_uint64(int(val))
                value_holders.append(v)
                val_ptr = ctypes.addressof(v)
            elif field['type'] == 'double':
                v = ctypes.c_double(float(val))
                value_holders.append(v)
                val_ptr = ctypes.addressof(v)
            elif field['type'] == 'string':
                v = str(val).encode('utf-8')
                value_holders.append(v)
                val_ptr = ctypes.c_char_p(v)
            else:
                raise ValueError(f"Unsupported type for where: {field['type']}")
            
            conditions.append({
                'field_offset': field['offset'],
                'field_size': field['size'],
                'value': val_ptr,
                'op': op,
            })
        
        if not conditions:
            return self.select()
        
        conds = (db_condition_t * len(conditions))()
        for i, c in enumerate(conditions):
            conds[i].field_offset = c['field_offset']
            conds[i].field_size = c['field_size']
            conds[i].value = c['value']
            conds[i].op = c['op']
        
        ptr = _lib.db_select_where_json(self._ptr, conds, len(conditions))
        return _parse_json_result(ptr)
    
    def update(self, id, row):
        """更新一行"""
        buf = _serialize_row(row, self._layout['fields'], self._layout['row_size'])
        buf_ptr = ctypes.create_string_buffer(buf)
        return _lib.db_update(self._ptr, id, buf_ptr, self._layout['row_size'])
    
    def delete(self, id):
        """删除一行"""
        return _lib.db_delete(self._ptr, id)
    
    def count(self):
        """获取行数"""
        return _lib.db_table_count(self._ptr)
    
    def create_index(self, field_name):
        """创建索引"""
        field = self._layout['field_map'].get(field_name)
        if not field:
            raise ValueError(f"Unknown field: {field_name}")
        return _lib.db_table_add_index(
            self._ptr, field_name.encode('utf-8'),
            field['offset'], TYPE_MAP[field['type']]
        )


class DB:
    """数据库对象"""
    
    def __init__(self, ptr):
        self._ptr = ptr
        self._tables = {}
    
    def register(self, name, schema):
        """注册表
        
        schema: [{name: str, type: str, size?: int}, ...]
        """
        fields, row_size = _compute_layout(schema)
        
        cfields = (db_field_def_t * len(fields))()
        for i, f in enumerate(fields):
            cfields[i].name = f['name'].encode('utf-8')
            cfields[i].offset = f['offset']
            cfields[i].size = f['size']
            cfields[i].type = TYPE_MAP[f['type']]
        
        ptr = _lib.db_table_register(
            self._ptr, name.encode('utf-8'),
            row_size, cfields, len(fields)
        )
        
        if not ptr:
            raise RuntimeError(f"Failed to register table: {name}")
        
        field_map = {f['name']: f for f in fields}
        layout = {
            'fields': fields,
            'field_map': field_map,
            'row_size': row_size,
        }
        
        self._tables[name] = layout
        return Table(ptr, layout, name)
    
    def table(self, name):
        """获取已注册的表"""
        ptr = _lib.db_table(self._ptr, name.encode('utf-8'))
        if not ptr:
            raise ValueError(f"Table not found: {name}")
        
        layout = self._tables.get(name)
        if not layout:
            raise ValueError(f"Table not registered in this session: {name}")
        
        return Table(ptr, layout, name)
    
    def sync(self):
        """强制刷盘（同步写入磁盘）"""
        return _lib.db_sync(self._ptr)
    
    def checkpoint(self):
        """执行 Checkpoint"""
        return _lib.db_checkpoint(self._ptr)
    
    def close(self):
        """关闭数据库"""
        if self._ptr:
            _lib.db_close(self._ptr)
            self._ptr = None
    
    def max_rows(self, n):
        """设置最大返回行数"""
        _lib.db_config_max_rows(self._ptr, n)
    
    def __enter__(self):
        return self
    
    def __exit__(self, *args):
        self.close()
        return False


def open(data_path="mydb_data.bin", index_path=None, wal_path=None, pool_size=100*1024*1024):
    """打开数据库"""
    index_path = index_path or data_path + ".index"
    wal_path = wal_path or data_path + ".wal"
    
    ptr = _lib.db_open(
        data_path.encode('utf-8'),
        index_path.encode('utf-8'),
        wal_path.encode('utf-8'),
        pool_size
    )
    
    if not ptr:
        raise RuntimeError("Failed to open database")
    
    return DB(ptr)


def create(path, schemas=None):
    """创建数据库并注册表
    
    schemas: {table_name: [field_def, ...], ...}
    """
    db = open(path)
    
    if schemas:
        for name, schema in schemas.items():
            db.register(name, schema)
    
    return db
