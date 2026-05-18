#!/usr/bin/env python3
"""
测试二进制向量存储性能
对比 JSON vs Raw Binary
"""
import os, sys, json, time, struct

sys.path.insert(0, '/home/dministrator/my_db')
from mydb.cache import open_cache

cache = open_cache('./test_binary_perf')
DIM = 384

# 生成测试数据
import random
random.seed(42)
test_vectors = []
for i in range(100):
    vec = [random.uniform(-1, 1) for _ in range(DIM)]
    test_vectors.append((f"func_{i}", vec))

print(f"Testing with {len(test_vectors)} vectors")
print(f"JSON size: ~{DIM * 10} bytes/vector")
print(f"Binary size: {DIM * 4} bytes/vector")
print()

# 测试 1: JSON 存储
print("Test 1: JSON storage...")
start = time.time()
for name, vec in test_vectors:
    cache.set_json(f"json/{name}", {"name": name, "vector": vec}, 0)
cache.sync()
json_time = time.time() - start
print(f"  Time: {json_time:.3f}s")
print(f"  Rate: {len(test_vectors)/json_time:.1f} items/s")

# 测试 2: Binary 存储
print("\nTest 2: Binary storage...")
start = time.time()
for name, vec in test_vectors:
    # Pack as raw bytes: 384 floats
    data = struct.pack(f'{DIM}f', *vec)
    import base64
    cache.set(f"bin/{name}", base64.b64encode(data).decode(), 0)
cache.sync()
bin_time = time.time() - start
print(f"  Time: {bin_time:.3f}s")
print(f"  Rate: {len(test_vectors)/bin_time:.1f} items/s")
print(f"  Speedup: {json_time/bin_time:.1f}x")

# 测试 3: 验证读取
print("\nTest 3: Verify binary read...")
key = f"bin/{test_vectors[0][0]}"
data = cache.get(key)
if data:
    import base64
    vec = struct.unpack(f'{DIM}f', base64.b64decode(data))
    original = test_vectors[0][1]
    diff = max(abs(a-b) for a,b in zip(vec, original))
    print(f"  Max diff: {diff:.10f}")
    print(f"  Read OK: {diff < 1e-6}")

# 测试 4: 批量 binary (no intermediate sync)
print("\nTest 4: Binary batch without sync...")
start = time.time()
for name, vec in test_vectors:
    data = struct.pack(f'{DIM}f', *vec)
    import base64
    cache.set(f"batch/{name}", base64.b64encode(data).decode(), 0)
cache.sync()
batch_time = time.time() - start
print(f"  Time: {batch_time:.3f}s")
print(f"  Rate: {len(test_vectors)/batch_time:.1f} items/s")

print(f"\n{'='*50}")
print(f"Summary: Binary is {json_time/bin_time:.1f}x faster than JSON")
print(f"Binary rate: {len(test_vectors)/bin_time:.1f} items/s")
print(f"Expected 41K vectors: {41000/(len(test_vectors)/bin_time):.1f}s")
