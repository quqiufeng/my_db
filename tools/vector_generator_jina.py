#!/usr/bin/env python3
"""
Generate semantic vectors using Jina Embeddings v2 base code (Python version)
Outputs compatible .bin and .idx files for C vector_search tool
"""

import sys
import os
import json
import struct
import numpy as np
from tokenizers import Tokenizer
import onnxruntime as ort

sys.path.insert(0, '/home/dministrator/my_db')

DIM = 768
MAX_SEQ_LEN = 512
BATCH_SIZE = 64

def load_model():
    model_path = "/home/dministrator/my_db/models/jina-embeddings-v2-base-code/model.onnx"
    tokenizer_path = "/home/dministrator/my_db/models/jina-embeddings-v2-base-code/tokenizer.json"
    
    tokenizer = Tokenizer.from_file(tokenizer_path)
    sess = ort.InferenceSession(model_path, providers=['CPUExecutionProvider'])
    
    return tokenizer, sess

def encode_batch(tokenizer, sess, texts):
    batch_size = len(texts)
    
    input_ids = np.zeros((batch_size, MAX_SEQ_LEN), dtype=np.int64)
    attention_mask = np.zeros((batch_size, MAX_SEQ_LEN), dtype=np.int64)
    
    for i, text in enumerate(texts):
        encoded = tokenizer.encode(text)
        ids = encoded.ids[:MAX_SEQ_LEN]
        length = len(ids)
        input_ids[i, :length] = ids
        attention_mask[i, :length] = 1
    
    outputs = sess.run(None, {
        'input_ids': input_ids,
        'attention_mask': attention_mask,
    })
    
    hidden = outputs[0]
    vectors = np.zeros((batch_size, DIM), dtype=np.float32)
    
    for i in range(batch_size):
        valid_len = int(attention_mask[i].sum())
        vec = hidden[i, :valid_len].mean(axis=0)
        norm = np.linalg.norm(vec)
        if norm > 1e-6:
            vec = vec / norm
        vectors[i] = vec
    
    return vectors

def generate_vectors(cache_dir, namespace=None):
    print(f"Collecting chunks from namespace: {namespace}")
    chunk_prefix = f"{namespace}/chunks/"
    
    # Collect items from cache
    import ctypes
    lib = ctypes.CDLL('/home/dministrator/my_db/libmydb.so')
    cache = lib.cache_open(cache_dir.encode(), 1024*1024*1024)
    iter_ptr = lib.cache_iter_create(cache)
    
    key_ptr = ctypes.c_char_p()
    val_ptr = ctypes.c_char_p()
    
    items = []
    while lib.cache_iter_next(iter_ptr, ctypes.byref(key_ptr), ctypes.byref(val_ptr)) == 1:
        key = key_ptr.value.decode('utf-8')
        if key.startswith(chunk_prefix):
            val = val_ptr.value.decode('utf-8')
            try:
                data = json.loads(val)
                if data.get('kind') in ['function', 'method', 'class', 'struct', 'macro']:
                    text = ""
                    if data.get('signature'):
                        text += data['signature'] + "\n"
                    if data.get('docstring'):
                        text += data['docstring'] + "\n"
                    text += data.get('content', '')
                    
                    name = key.split('/')[-1]
                    items.append({'name': name, 'text': text[:4000]})
            except:
                pass
    
    print(f"Found {len(items)} items to encode")
    
    if not items:
        print("Nothing to do")
        return
    
    # Encode
    print("Loading Jina model...")
    tokenizer, sess = load_model()
    
    # Encode
    print("Encoding with Jina...")
    import time
    start_time = time.time()
    
    all_vectors = []
    for i in range(0, len(items), BATCH_SIZE):
        batch = items[i:i+BATCH_SIZE]
        texts = [item['text'] for item in batch]
        vectors = encode_batch(tokenizer, sess, texts)
        all_vectors.append(vectors)
        
        if (i // BATCH_SIZE + 1) % 10 == 0:
            print(f"  Progress: {min(i + BATCH_SIZE, len(items))}/{len(items)}")
    
    elapsed = time.time() - start_time
    total = len(items)
    print(f"Encoded {total}/{total} items in {elapsed:.1f}s ({total/elapsed:.1f} items/s)")
    
    all_vectors = np.vstack(all_vectors)
    
    # Write files
    ns_safe = namespace.replace('/', '_').replace('-', '_')
    vec_dir = os.path.join(cache_dir, "vectors")
    os.makedirs(vec_dir, exist_ok=True)
    
    vec_file = os.path.join(vec_dir, f"{ns_safe}.jina.bin")
    idx_file = os.path.join(vec_dir, f"{ns_safe}.jina.idx")
    
    with open(vec_file, 'wb') as f:
        f.write(struct.pack('<II', len(items), DIM))
        f.write(all_vectors.tobytes())
    
    idx = {}
    offset = 8
    for item in items:
        idx[item['name']] = offset
        offset += DIM * 4
    
    with open(idx_file, 'w') as f:
        json.dump(idx, f)
    
    print(f"Binary file: {vec_file}")
    print(f"Index file: {idx_file}")
    print(f"Stored {len(items)} vectors")
    print("Done!")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 vector_generator_jina.py <cache_dir> [namespace]")
        sys.exit(1)
    
    cache_dir = sys.argv[1]
    namespace = sys.argv[2] if len(sys.argv) > 2 else None
    
    generate_vectors(cache_dir, namespace)
