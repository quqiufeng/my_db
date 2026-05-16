#!/usr/bin/env python3
"""
mydb/cache.py - Python ctypes 封装 for KV Cache
让 KV Cache 像原生 Python 字典一样使用
"""

import ctypes
import os
import json

# 加载动态库（复用 mydb.py 的库路径）
lib_paths = [
    "./libmydb.so",
    "libmydb.so", 
    "./libcache.so",
    "libcache.so",
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
    raise RuntimeError(f"Cannot load KV Cache library, tried: {lib_paths}")

# ========================================================================
# 配置函数签名
# ========================================================================

# cache_open(const char*, size_t) -> cache_t*
_lib.cache_open.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
_lib.cache_open.restype = ctypes.c_void_p

# cache_close(cache_t*)
_lib.cache_close.argtypes = [ctypes.c_void_p]

# cache_sync(cache_t*) -> int
_lib.cache_sync.argtypes = [ctypes.c_void_p]
_lib.cache_sync.restype = ctypes.c_int

# cache_set(cache_t*, const char*, const char*, uint64_t) -> int
_lib.cache_set.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint64]
_lib.cache_set.restype = ctypes.c_int

# cache_get(cache_t*, const char*) -> const char*
_lib.cache_get.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
_lib.cache_get.restype = ctypes.c_char_p

# cache_del(cache_t*, const char*) -> int
_lib.cache_del.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
_lib.cache_del.restype = ctypes.c_int

# cache_exists(cache_t*, const char*) -> int
_lib.cache_exists.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
_lib.cache_exists.restype = ctypes.c_int

# cache_count(cache_t*) -> size_t
_lib.cache_count.argtypes = [ctypes.c_void_p]
_lib.cache_count.restype = ctypes.c_size_t

# cache_memory_used(cache_t*) -> size_t
_lib.cache_memory_used.argtypes = [ctypes.c_void_p]
_lib.cache_memory_used.restype = ctypes.c_size_t

# cache_memory_max(cache_t*) -> size_t
_lib.cache_memory_max.argtypes = [ctypes.c_void_p]
_lib.cache_memory_max.restype = ctypes.c_size_t

# cache_set_ns(cache_t*, const char*, const char*, const char*, uint64_t) -> int
_lib.cache_set_ns.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint64]
_lib.cache_set_ns.restype = ctypes.c_int

# cache_get_ns(cache_t*, const char*, const char*) -> const char*
_lib.cache_get_ns.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
_lib.cache_get_ns.restype = ctypes.c_char_p

# cache_del_ns(cache_t*, const char*, const char*) -> int
_lib.cache_del_ns.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
_lib.cache_del_ns.restype = ctypes.c_int

# cache_expire(cache_t*, const char*) -> int
_lib.cache_expire.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
_lib.cache_expire.restype = ctypes.c_int

# cache_touch(cache_t*, const char*) -> int
_lib.cache_touch.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
_lib.cache_touch.restype = ctypes.c_int

# cache_compact(cache_t*) -> size_t
_lib.cache_compact.argtypes = [ctypes.c_void_p]
_lib.cache_compact.restype = ctypes.c_size_t

# cache_purge_expired(cache_t*) -> size_t
_lib.cache_purge_expired.argtypes = [ctypes.c_void_p]
_lib.cache_purge_expired.restype = ctypes.c_size_t

# cache_check(const char*) -> int
_lib.cache_check.argtypes = [ctypes.c_char_p]
_lib.cache_check.restype = ctypes.c_int

# ========================================================================
# 搜索相关
# ========================================================================

class _CacheSearchOptions(ctypes.Structure):
    _fields_ = [
        ("max_results", ctypes.c_int),
        ("case_sensitive", ctypes.c_int),
        ("ns_filter", ctypes.c_char_p),
    ]

class _CacheResult(ctypes.Structure):
    _fields_ = [
        ("key", ctypes.c_char_p),
        ("value", ctypes.c_char_p),
        ("score", ctypes.c_double),
    ]

_lib.cache_search_options_default.argtypes = []
_lib.cache_search_options_default.restype = _CacheSearchOptions

# cache_search_prefix(cache_t*, const char*, options*, result**, count*) -> int
_lib.cache_search_prefix.argtypes = [
    ctypes.c_void_p, ctypes.c_char_p,
    ctypes.POINTER(_CacheSearchOptions),
    ctypes.POINTER(ctypes.POINTER(_CacheResult)),
    ctypes.POINTER(ctypes.c_size_t)
]
_lib.cache_search_prefix.restype = ctypes.c_int

# cache_search_range
_lib.cache_search_range.argtypes = [
    ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
    ctypes.POINTER(_CacheSearchOptions),
    ctypes.POINTER(ctypes.POINTER(_CacheResult)),
    ctypes.POINTER(ctypes.c_size_t)
]
_lib.cache_search_range.restype = ctypes.c_int

# cache_search_regex
_lib.cache_search_regex.argtypes = [
    ctypes.c_void_p, ctypes.c_char_p,
    ctypes.POINTER(_CacheSearchOptions),
    ctypes.POINTER(ctypes.POINTER(_CacheResult)),
    ctypes.POINTER(ctypes.c_size_t)
]
_lib.cache_search_regex.restype = ctypes.c_int

# cache_search_fuzzy
_lib.cache_search_fuzzy.argtypes = [
    ctypes.c_void_p, ctypes.c_char_p,
    ctypes.POINTER(_CacheSearchOptions),
    ctypes.POINTER(ctypes.POINTER(_CacheResult)),
    ctypes.POINTER(ctypes.c_size_t)
]
_lib.cache_search_fuzzy.restype = ctypes.c_int

# cache_search_tag
_lib.cache_search_tag.argtypes = [
    ctypes.c_void_p, ctypes.c_char_p,
    ctypes.POINTER(_CacheSearchOptions),
    ctypes.POINTER(ctypes.POINTER(_CacheResult)),
    ctypes.POINTER(ctypes.c_size_t)
]
_lib.cache_search_tag.restype = ctypes.c_int

# cache_results_free(cache_result_t*)
_lib.cache_results_free.argtypes = [ctypes.POINTER(_CacheResult)]

# ========================================================================
# 迭代器
# ========================================================================

_lib.cache_iter_create.argtypes = [ctypes.c_void_p]
_lib.cache_iter_create.restype = ctypes.c_void_p

_lib.cache_iter_destroy.argtypes = [ctypes.c_void_p]

_lib.cache_iter_next.argtypes = [
    ctypes.c_void_p,
    ctypes.POINTER(ctypes.c_char_p),
    ctypes.POINTER(ctypes.c_char_p)
]
_lib.cache_iter_next.restype = ctypes.c_int

_lib.cache_iter_reset.argtypes = [ctypes.c_void_p]

# ========================================================================
# Python 类封装
# ========================================================================

class Cache:
    """KV Cache - Pythonic wrapper around my_db cache_t"""
    
    def __init__(self, ptr):
        self._ptr = ptr
    
    def set(self, key, value, ttl_ms=0):
        """设置 key-value，ttl_ms=0 表示永久"""
        ret = _lib.cache_set(self._ptr, key.encode('utf-8'), value.encode('utf-8'), ttl_ms)
        if ret != 0:
            raise RuntimeError(f"cache_set failed: {ret}")
        return self
    
    def get(self, key):
        """获取 value，不存在返回 None"""
        ptr = _lib.cache_get(self._ptr, key.encode('utf-8'))
        if not ptr:
            return None
        return ptr.decode('utf-8', errors='ignore')
    
    def __getitem__(self, key):
        """支持 cache['key'] 语法"""
        value = self.get(key)
        if value is None:
            raise KeyError(key)
        return value
    
    def __setitem__(self, key, value):
        """支持 cache['key'] = value 语法"""
        self.set(key, value)
    
    def __contains__(self, key):
        """支持 'key' in cache 语法"""
        return _lib.cache_exists(self._ptr, key.encode('utf-8')) != 0
    
    def delete(self, key):
        """删除 key"""
        ret = _lib.cache_del(self._ptr, key.encode('utf-8'))
        if ret == -4:  # CACHE_ERR_NOENT
            raise KeyError(key)
        if ret != 0:
            raise RuntimeError(f"cache_del failed: {ret}")
    
    def __delitem__(self, key):
        """支持 del cache['key'] 语法"""
        self.delete(key)
    
    def set_json(self, key, obj, ttl_ms=0):
        """设置 JSON 对象（自动序列化）"""
        return self.set(key, json.dumps(obj, ensure_ascii=False), ttl_ms)
    
    def get_json(self, key):
        """获取 JSON 对象（自动反序列化）"""
        value = self.get(key)
        if value is None:
            return None
        return json.loads(value)
    
    # Namespace 操作
    def set_ns(self, ns, key, value, ttl_ms=0):
        """在 namespace 内设置"""
        ret = _lib.cache_set_ns(self._ptr, ns.encode('utf-8'), key.encode('utf-8'), 
                                value.encode('utf-8'), ttl_ms)
        if ret != 0:
            raise RuntimeError(f"cache_set_ns failed: {ret}")
    
    def get_ns(self, ns, key):
        """在 namespace 内获取"""
        ptr = _lib.cache_get_ns(self._ptr, ns.encode('utf-8'), key.encode('utf-8'))
        if not ptr:
            return None
        return ptr.decode('utf-8', errors='ignore')
    
    def delete_ns(self, ns, key):
        """在 namespace 内删除"""
        ret = _lib.cache_del_ns(self._ptr, ns.encode('utf-8'), key.encode('utf-8'))
        if ret == -4:
            raise KeyError(f"{ns}/{key}")
        if ret != 0:
            raise RuntimeError(f"cache_del_ns failed: {ret}")
    
    # 搜索
    def search_prefix(self, prefix, max_results=100, ns_filter=None):
        """前缀搜索"""
        opts = _CacheSearchOptions()
        opts.max_results = max_results
        opts.case_sensitive = 0
        opts.ns_filter = ns_filter.encode('utf-8') if ns_filter else None
        
        results_ptr = ctypes.POINTER(_CacheResult)()
        count = ctypes.c_size_t()
        
        ret = _lib.cache_search_prefix(
            self._ptr, prefix.encode('utf-8'),
            ctypes.byref(opts),
            ctypes.byref(results_ptr),
            ctypes.byref(count)
        )
        
        if ret != 0:
            return []
        
        results = []
        for i in range(count.value):
            r = results_ptr[i]
            key = r.key.decode('utf-8', errors='ignore') if r.key else ""
            value = r.value.decode('utf-8', errors='ignore') if r.value else ""
            results.append({'key': key, 'value': value, 'score': r.score})
        
        if results_ptr:
            _lib.cache_results_free(results_ptr)
        
        return results
    
    def search_regex(self, pattern, max_results=100, case_sensitive=False):
        """正则搜索"""
        opts = _CacheSearchOptions()
        opts.max_results = max_results
        opts.case_sensitive = 1 if case_sensitive else 0
        opts.ns_filter = None
        
        results_ptr = ctypes.POINTER(_CacheResult)()
        count = ctypes.c_size_t()
        
        ret = _lib.cache_search_regex(
            self._ptr, pattern.encode('utf-8'),
            ctypes.byref(opts),
            ctypes.byref(results_ptr),
            ctypes.byref(count)
        )
        
        if ret != 0:
            return []
        
        results = []
        for i in range(count.value):
            r = results_ptr[i]
            key = r.key.decode('utf-8', errors='ignore') if r.key else ""
            value = r.value.decode('utf-8', errors='ignore') if r.value else ""
            results.append({'key': key, 'value': value, 'score': r.score})
        
        if results_ptr:
            _lib.cache_results_free(results_ptr)
        
        return results
    
    def search_fuzzy(self, query, max_results=100):
        """模糊搜索"""
        opts = _CacheSearchOptions()
        opts.max_results = max_results
        opts.case_sensitive = 0
        opts.ns_filter = None
        
        results_ptr = ctypes.POINTER(_CacheResult)()
        count = ctypes.c_size_t()
        
        ret = _lib.cache_search_fuzzy(
            self._ptr, query.encode('utf-8'),
            ctypes.byref(opts),
            ctypes.byref(results_ptr),
            ctypes.byref(count)
        )
        
        if ret != 0:
            return []
        
        results = []
        for i in range(count.value):
            r = results_ptr[i]
            key = r.key.decode('utf-8', errors='ignore') if r.key else ""
            value = r.value.decode('utf-8', errors='ignore') if r.value else ""
            results.append({'key': key, 'value': value, 'score': r.score})
        
        if results_ptr:
            _lib.cache_results_free(results_ptr)
        
        return results
    
    def search_tag(self, tag, max_results=100):
        """标签搜索"""
        opts = _CacheSearchOptions()
        opts.max_results = max_results
        opts.case_sensitive = 0
        opts.ns_filter = None
        
        results_ptr = ctypes.POINTER(_CacheResult)()
        count = ctypes.c_size_t()
        
        ret = _lib.cache_search_tag(
            self._ptr, tag.encode('utf-8'),
            ctypes.byref(opts),
            ctypes.byref(results_ptr),
            ctypes.byref(count)
        )
        
        if ret != 0:
            return []
        
        results = []
        for i in range(count.value):
            r = results_ptr[i]
            key = r.key.decode('utf-8', errors='ignore') if r.key else ""
            value = r.value.decode('utf-8', errors='ignore') if r.value else ""
            results.append({'key': key, 'value': value, 'score': r.score})
        
        if results_ptr:
            _lib.cache_results_free(results_ptr)
        
        return results
    
    # 迭代器
    def items(self):
        """遍历所有 entry（按 key 排序）"""
        iter_ptr = _lib.cache_iter_create(self._ptr)
        if not iter_ptr:
            return
        
        try:
            key_ptr = ctypes.c_char_p()
            value_ptr = ctypes.c_char_p()
            
            while _lib.cache_iter_next(iter_ptr, ctypes.byref(key_ptr), ctypes.byref(value_ptr)):
                if key_ptr.value and value_ptr.value:
                    yield (
                        key_ptr.value.decode('utf-8', errors='ignore'),
                        value_ptr.value.decode('utf-8', errors='ignore')
                    )
        finally:
            _lib.cache_iter_destroy(iter_ptr)
    
    def keys(self):
        """所有 key"""
        for k, _ in self.items():
            yield k
    
    def values(self):
        """所有 value"""
        for _, v in self.items():
            yield v
    
    # 管理操作
    def expire(self, key):
        """立即过期"""
        ret = _lib.cache_expire(self._ptr, key.encode('utf-8'))
        if ret == -4:
            raise KeyError(key)
        if ret != 0:
            raise RuntimeError(f"cache_expire failed: {ret}")
    
    def touch(self, key):
        """更新访问时间"""
        ret = _lib.cache_touch(self._ptr, key.encode('utf-8'))
        if ret == -4:
            raise KeyError(key)
        if ret != 0:
            raise RuntimeError(f"cache_touch failed: {ret}")
    
    def compact(self):
        """物理清理"""
        return _lib.cache_compact(self._ptr)
    
    def purge_expired(self):
        """清理过期条目"""
        return _lib.cache_purge_expired(self._ptr)
    
    def sync(self):
        """强制刷盘"""
        return _lib.cache_sync(self._ptr)
    
    def close(self):
        """关闭 cache"""
        if self._ptr:
            _lib.cache_close(self._ptr)
            self._ptr = None
    
    @property
    def count(self):
        """条目数"""
        return _lib.cache_count(self._ptr)
    
    @property
    def memory_used(self):
        """已用内存"""
        return _lib.cache_memory_used(self._ptr)
    
    @property
    def memory_max(self):
        """最大内存"""
        return _lib.cache_memory_max(self._ptr)
    
    def __enter__(self):
        return self
    
    def __exit__(self, *args):
        self.close()
        return False
    
    def __repr__(self):
        return f"Cache(count={self.count}, memory={self.memory_used}/{self.memory_max})"
    
    def __len__(self):
        return self.count


def open_cache(db_dir="cache_data", max_memory=100*1024*1024):
    """打开 KV Cache
    
    Args:
        db_dir: cache 目录
        max_memory: 最大内存（字节）
    
    Returns:
        Cache 实例
    """
    ptr = _lib.cache_open(db_dir.encode('utf-8'), max_memory)
    if not ptr:
        raise RuntimeError(f"Failed to open cache: {db_dir}")
    return Cache(ptr)


# 便捷函数
def check(db_dir):
    """检查 cache 文件完整性"""
    return _lib.cache_check(db_dir.encode('utf-8')) == 0


if __name__ == "__main__":
    print("=== KV Cache Python FFI Test ===\n")
    
    import tempfile
    import shutil
    
    tmpdir = tempfile.mkdtemp(prefix="cache_test_")
    
    try:
        # 打开 cache
        cache = open_cache(tmpdir, 10*1024*1024)
        print(f"1. Open: {cache}")
        
        # 基础 CRUD
        cache["hello"] = "world"
        assert cache["hello"] == "world"
        print(f"2. Set/Get: OK")
        
        # namespace
        cache.set_ns("/coding/cpp", "move", "右值引用...")
        assert cache.get_ns("/coding/cpp", "move") == "右值引用..."
        print(f"3. Namespace: OK")
        
        # JSON
        cache.set_json("/config", {"debug": True, "port": 8080})
        cfg = cache.get_json("/config")
        assert cfg["debug"] == True
        print(f"4. JSON: OK")
        
        # 搜索
        cache["apple"] = "fruit"
        cache["application"] = "software"
        cache["apply"] = "verb"
        
        results = cache.search_prefix("app")
        print(f"5. Prefix search 'app': {len(results)} results")
        for r in results[:3]:
            print(f"   - {r['key']}")
        
        # 迭代
        count = 0
        for k, v in cache.items():
            count += 1
        print(f"6. Iteration: {count} items")
        
        # 统计
        print(f"7. Stats: {cache}")
        
        cache.close()
        print("\n=== All Tests Passed ===")
        
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)
