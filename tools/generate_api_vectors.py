#!/usr/bin/env python3
"""
为 stable-diffusion.cpp 核心 API 生成语义向量
只生成关键函数（~50个），避免 cache 存储性能问题
"""

import os, sys, json, time, pickle

sys.path.insert(0, '/home/dministrator/my_db')
from mydb.cache import open_cache
from mydb.onnx_embedder import OnnxEmbedder

cache = open_cache('./ai_code_memory')
namespace = '/code/local/stable-diffusion.cpp'

# 收集核心 API 函数
print("Collecting core API functions...")
api_functions = []

for key in cache.keys():
    if not key.startswith(f'{namespace}/chunks/include/stable-diffusion.h/'):
        continue
    
    name = key.split('/')[-1]
    data = json.loads(cache.get(key))
    
    # 构建描述文本
    text = f"Function: {name}\n"
    text += f"Signature: {data.get('signature', '')}\n"
    text += f"Documentation: {data.get('docstring', '')}\n"
    content = data.get('content', '')
    if content:
        text += content[:500]
    
    api_functions.append({
        'name': name,
        'text': text,
        'file': 'include/stable-diffusion.h',
        'line': data.get('line_start')
    })

print(f"Found {len(api_functions)} API functions")

# 生成向量
print("Generating vectors...")
embedder = OnnxEmbedder()

batch_size = 32
results = []

for i in range(0, len(api_functions), batch_size):
    batch = api_functions[i:i+batch_size]
    texts = [f['text'] for f in batch]
    names = [f['name'] for f in batch]
    
    vectors = embedder.encode_batch(texts)
    for name, vector in zip(names, vectors):
        results.append({'name': name, 'vector': vector})
    
    if (i // batch_size) % 5 == 0:
        print(f"  {len(results)}/{len(api_functions)}")

print(f"Generated {len(results)} vectors")

# 存储到 cache（少量数据应该没问题）
print("Storing vectors...")
for item in results:
    cache.set_json(f"{namespace}/vectors/{item['name']}", {
        'name': item['name'],
        'vector': item['vector'],
    }, 0)

cache.sync()
print("Done!")

# 验证
vec_count = 0
for key in cache.keys():
    if key.startswith(f'{namespace}/vectors/'):
        vec_count += 1
print(f"Total vectors in cache: {vec_count}")
