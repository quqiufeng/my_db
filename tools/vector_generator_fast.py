#!/usr/bin/env python3
"""
高速批量向量生成器 - TensorRT GPU 加速

设计：
- 多线程读取 cache 数据（I/O 密集型）
- 单线程 batch GPU 推理（避免 ONNX session 线程安全问题）
- 大 batch size（64-128）充分利用 GPU
- 异步存储结果

性能目标：stable-diffusion.cpp 41K chunks → < 3 分钟
"""

import os
import sys
import json
import time
import threading
import queue
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from mydb.cache import open_cache
from mydb.onnx_embedder import OnnxEmbedder

BATCH_SIZE = 64  # RTX 3080 最优 batch
MAX_QUEUE = 256  # 预读取队列大小


def vector_generator(cache_dir: str, namespace: str, max_items: int = 50000):
    """批量生成语义向量"""
    
    print(f"Opening cache: {cache_dir}")
    cache = open_cache(cache_dir)
    
    # 收集待编码的 chunks（先全部读取到内存，避免推理时同时读写 cache）
    print("Collecting chunks...")
    items = []
    chunk_prefix = f"{namespace}/chunks/"
    existing_vectors = set()
    
    # 先收集已有向量
    for key in cache.keys():
        if key.startswith(f"{namespace}/vectors/"):
            name = key.split('/')[-1]
            existing_vectors.add(name)
    
    print(f"  Existing vectors: {len(existing_vectors)}")
    
    # 收集待编码 chunks
    for key in cache.keys():
        if not key.startswith(chunk_prefix):
            continue
        
        name = key.split('/')[-1]
        if name in existing_vectors:
            continue
        
        try:
            data = json.loads(cache.get(key))
            # 构建文本：签名 + 文档 + 内容预览
            text = f"{data.get('signature', '')}\n{data.get('docstring', '')}\n"
            content = data.get('content', '')
            if content:
                lines = content.split('\n')[:5]
                text += '\n'.join(lines)
            
            if len(text.strip()) > 10:
                items.append((name, text.strip()))
        except:
            pass
        
        if len(items) >= max_items:
            break
    
    total = len(items)
    if total == 0:
        print("No new vectors to generate")
        return
    
    print(f"Found {total} items to encode")
    print(f"Batch size: {BATCH_SIZE}")
    
    # 关闭 cache，避免推理时文件操作冲突
    del cache
    
    print("Loading TensorRT embedder...")
    embedder = OnnxEmbedder()
    
    # 批处理（纯推理，不操作 cache）
    results = []
    processed = 0
    start_time = time.time()
    
    for i in range(0, total, BATCH_SIZE):
        batch = items[i:i + BATCH_SIZE]
        texts = [text for _, text in batch]
        names = [name for name, _ in batch]
        
        # Batch encode
        try:
            vectors = embedder.encode_batch(texts)
            for name, vector in zip(names, vectors):
                results.append((name, vector))
            processed += len(batch)
        except Exception as e:
            print(f"  Batch error: {e}")
            # Fallback: single encode
            for name, text in batch:
                try:
                    vector = embedder.encode(text)
                    results.append((name, vector))
                    processed += 1
                except:
                    pass
        
        # 进度报告
        if (i // BATCH_SIZE) % 10 == 0 or processed >= total:
            elapsed = time.time() - start_time
            rate = processed / elapsed if elapsed > 0 else 0
            pct = processed / total * 100
            print(f"  Progress: {processed}/{total} ({pct:.1f}%) - {rate:.1f} items/s")
    
    # 重新打开 cache 存储结果
    print("Storing vectors to cache...")
    cache = open_cache(cache_dir)
    stored = 0
    for name, vector in results:
        cache.set_json(f"{namespace}/vectors/{name}", {
            "name": name,
            "vector": vector,
        }, 0)
        stored += 1
        if stored % 1000 == 0:
            cache.sync()
    
    cache.sync()
    
    elapsed = time.time() - start_time
    print(f"\nDone!")
    print(f"  Total: {processed}/{total}")
    print(f"  Time: {elapsed:.1f}s")
    print(f"  Rate: {processed/elapsed:.1f} items/s")
    
    # 显存报告
    try:
        import subprocess
        result = subprocess.run(['nvidia-smi', '--query-gpu=memory.used', '--format=csv,noheader,nounits'],
                              capture_output=True, text=True)
        mem_used = result.stdout.strip()
        print(f"  GPU Memory: {mem_used} MiB")
    except:
        pass


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <cache_dir> [namespace]")
        sys.exit(1)
    
    cache_dir = sys.argv[1]
    namespace = sys.argv[2] if len(sys.argv) > 2 else "/code/local/stable-diffusion.cpp"
    
    vector_generator(cache_dir, namespace)
