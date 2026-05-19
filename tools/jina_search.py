#!/usr/bin/env python3
"""Jina semantic search using pre-generated vectors"""
import sys
import numpy as np
from tokenizers import Tokenizer
import onnxruntime as ort
import json
import struct
import os

if len(sys.argv) < 3:
    print("Usage: python3 jina_search.py <cache_dir> <query> [top_k] [namespace]")
    sys.exit(1)

cache_dir = sys.argv[1]
query = sys.argv[2]
top_k = int(sys.argv[3]) if len(sys.argv) > 3 else 5
namespace = sys.argv[4] if len(sys.argv) > 4 else '/code/local/stable-diffusion.cpp'

# Find actual vector file
vec_dir = f"{cache_dir}/vectors"
ns_prefix = namespace.replace('/', '_').lstrip('_')
vec_files = [f for f in os.listdir(vec_dir) if f.startswith(ns_prefix) and f.endswith('.jina.bin')]
if not vec_files:
    print(f"No Jina vectors found for {namespace}")
    sys.exit(1)

vec_file = os.path.join(vec_dir, vec_files[0])
idx_file = vec_file.replace('.bin', '.idx')

print(f"Loading Jina model...")
tokenizer = Tokenizer.from_file('/home/dministrator/my_db/models/jina-embeddings-v2-base-code/tokenizer.json')
sess = ort.InferenceSession('/home/dministrator/my_db/models/jina-embeddings-v2-base-code/model.onnx', providers=['CUDAExecutionProvider', 'CPUExecutionProvider'])

print(f"Loading vectors from {vec_file}...")
with open(vec_file, 'rb') as f:
    count = np.frombuffer(f.read(4), dtype=np.uint32)[0]
    dim = np.frombuffer(f.read(4), dtype=np.uint32)[0]
    doc_vectors = np.frombuffer(f.read(), dtype=np.float32).reshape(count, dim)

with open(idx_file, 'r') as f:
    idx_map = json.load(f)

names = [''] * count
for name, offset in idx_map.items():
    pos = (offset - 8) // (dim * 4)
    names[pos] = name

# Encode query
print(f"Encoding query: {query}")
encoded = tokenizer.encode(query)
ids = encoded.ids + [1] * (512 - len(encoded.ids))
mask = [1] * len(encoded.ids) + [0] * (512 - len(encoded.ids))

out = sess.run(None, {
    'input_ids': np.array([ids], dtype=np.int64),
    'attention_mask': np.array([mask], dtype=np.int64),
})

valid_len = sum(mask)
query_vec = out[0][0, :valid_len].mean(axis=0)
norm = np.linalg.norm(query_vec)
if norm > 1e-6:
    query_vec = query_vec / norm

# Search
print(f"Searching {count} vectors...")
sims = doc_vectors @ query_vec
top_indices = np.argsort(sims)[::-1][:top_k]

# Load cache for rich output
import ctypes
lib = ctypes.CDLL('/home/dministrator/my_db/libmydb.so')
cache = lib.cache_open(cache_dir.encode(), 1024*1024*1024)
cache_iter = lib.cache_iter_create(cache)

key_ptr = ctypes.c_char_p()
val_ptr = ctypes.c_char_p()

# Build symbol lookup
symbols = {}
while lib.cache_iter_next(cache_iter, ctypes.byref(key_ptr), ctypes.byref(val_ptr)) == 1:
    key = key_ptr.value.decode('utf-8')
    if key.startswith(f'{namespace}/chunks/'):
        val = json.loads(val_ptr.value.decode('utf-8'))
        name = key.split('/')[-1]
        symbols[name] = val

print(f"\nTop {top_k} results:\n")
for i, idx_pos in enumerate(top_indices, 1):
    name = names[idx_pos]
    score = sims[idx_pos]
    print(f"[{i}] {name} ({score:.4f})")
    
    sym = symbols.get(name, {})
    if sym.get('signature'):
        print(f"    Signature: {sym['signature'][:80]}")
    if sym.get('file'):
        print(f"    File: {sym['file']}:{sym.get('line_start', 0)}")
    if sym.get('content'):
        lines = sym['content'].split('\n')[:8]
        print(f"    Code:")
        for line in lines:
            print(f"      {line[:100]}")
    print()
