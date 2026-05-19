#!/usr/bin/env python3
"""
二进制向量存储 - 解决 cache 存储性能瓶颈

设计：
- 向量存储在独立的二进制文件 (vectors.bin)
- Cache 中只存偏移量索引 (name -> offset)
- 读取时直接 mmap 二进制文件
"""

import os
import sys
import json
import struct
import time

sys.path.insert(0, '/home/dministrator/my_db')
from mydb.cache import open_cache
from mydb.onnx_embedder import OnnxEmbedder

DIM = 768
BATCH_SIZE = 64
MAX_ITEMS = 50000


def generate_vectors(cache_dir, namespace, model_dir='models/all-mpnet-base-v2'):
    """生成语义向量并存储到二进制文件"""
    
    cache = open_cache(cache_dir)
    
    # Try to load from pickle first (for large datasets)
    pickle_file = '/tmp/sd_items.pkl'
    if os.path.exists(pickle_file):
        print(f"Loading items from {pickle_file}...")
        import pickle
        with open(pickle_file, 'rb') as f:
            items = pickle.load(f)
        print(f"Loaded {len(items)} items from pickle")
    else:
        # 收集待编码的 chunks
        print("Collecting chunks...")
        items = []
        chunk_prefix = f"{namespace}/chunks/"
        
        for key in cache.keys():
            if not key.startswith(chunk_prefix):
                continue
            
            name = key.split('/')[-1]
            vec_key = f"{namespace}/vectors/{name}"
            if cache.get(vec_key):
                continue
            
            try:
                data = json.loads(cache.get(key))
                text = f"{data.get('signature', '')}\n{data.get('docstring', '')}\n"
                content = data.get('content', '')
                if content:
                    text += '\n'.join(content.split('\n')[:5])
                
                if len(text.strip()) > 10:
                    items.append({'name': name, 'text': text.strip()})
            except:
                pass
            
            if len(items) >= MAX_ITEMS:
                break
    
    total = len(items)
    print(f"Found {total} items to encode")
    
    if total == 0:
        print("Nothing to do")
        return
    
    # 生成向量
    print(f"Encoding with batch={BATCH_SIZE}...")
    embedder = OnnxEmbedder()
    
    vectors = []
    start = time.time()
    
    for i in range(0, total, BATCH_SIZE):
        batch = items[i:i+BATCH_SIZE]
        texts = [item['text'] for item in batch]
        
        try:
            batch_vectors = embedder.encode_batch(texts)
            vectors.extend(batch_vectors)
        except Exception as e:
            print(f"Batch error at {i}: {e}")
            for item in batch:
                try:
                    v = embedder.encode(item['text'])
                    vectors.append(v)
                except:
                    vectors.append([0.0] * DIM)
        
        if (i // BATCH_SIZE) % 10 == 0:
            elapsed = time.time() - start
            rate = len(vectors) / elapsed if elapsed > 0 else 0
            print(f"  {len(vectors)}/{total} - {rate:.1f} items/s")
    
    encode_time = time.time() - start
    print(f"Encoded {len(vectors)} vectors in {encode_time:.1f}s ({len(vectors)/encode_time:.1f} items/s)")
    
    # 存储到二进制文件
    print("Storing to binary file...")
    store_start = time.time()
    
    # 文件路径
    vec_dir = os.path.join(cache_dir, 'vectors')
    os.makedirs(vec_dir, exist_ok=True)
    safe_ns = namespace.replace('/', '_').strip('_')
    vec_file = os.path.join(vec_dir, f"{safe_ns}.bin")
    idx_file = os.path.join(vec_dir, f"{safe_ns}.idx")
    
    # 写入二进制文件 [count:4][dim:4][vectors...]
    with open(vec_file, 'wb') as f:
        f.write(struct.pack('II', len(vectors), DIM))
        for vec in vectors:
            f.write(struct.pack(f'{DIM}f', *vec))
    
    # 写入索引文件 {name: offset}
    offset = 8  # skip header
    index = {}
    for i, item in enumerate(items):
        index[item['name']] = offset
        offset += DIM * 4
    
    with open(idx_file, 'w') as f:
        json.dump(index, f)
    
    # 同时在 cache 中存储偏移量索引（轻量级）
    for name, off in index.items():
        cache.set(f"{namespace}/vectors/{name}", f"bin:{off}", 0)
    
    cache.sync()
    
    store_time = time.time() - store_start
    print(f"Stored in {store_time:.1f}s")
    print(f"  Binary: {vec_file}")
    print(f"  Index: {idx_file}")
    print(f"  Total: {encode_time + store_time:.1f}s")
    
    # 验证
    file_size = os.path.getsize(vec_file)
    expected = 8 + len(vectors) * DIM * 4
    print(f"  File size: {file_size} bytes (expected: {expected})")
    print(f"  OK: {file_size == expected}")


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <cache_dir> [namespace]")
        sys.exit(1)
    
    cache_dir = sys.argv[1]
    namespace = sys.argv[2] if len(sys.argv) > 2 else '/code/local/stable-diffusion.cpp'
    
    generate_vectors(cache_dir, namespace)
