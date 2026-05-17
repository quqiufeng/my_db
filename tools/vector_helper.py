#!/usr/bin/env python3
"""
Vector embedding helper for my_db KV Cache.
Uses sentence-transformers to generate text embeddings.

Usage:
    python3 vector_helper.py embed "text to embed"
    python3 vector_helper.py search "query text" --db-dir ./my_cache
"""

import sys
import os
import json
import struct
import ctypes
from pathlib import Path

# Try to import sentence-transformers
try:
    from sentence_transformers import SentenceTransformer
    ST_AVAILABLE = True
except ImportError:
    ST_AVAILABLE = False
    print("Warning: sentence-transformers not available. Using random vectors.")

# Load mydb library
lib_path = os.path.join(os.path.dirname(__file__), '..', 'libmydb.so')
if not os.path.exists(lib_path):
    lib_path = './libmydb.so'

try:
    lib = ctypes.CDLL(lib_path)
except OSError:
    print(f"Error: Cannot load {lib_path}")
    sys.exit(1)

# Model cache
_model = None

def get_model():
    global _model
    if _model is None:
        if ST_AVAILABLE:
            print("Loading sentence-transformers model (all-MiniLM-L6-v2)...")
            _model = SentenceTransformer('all-MiniLM-L6-v2')
        else:
            return None
    return _model

def embed_text(text):
    """Generate embedding vector for text."""
    model = get_model()
    if model:
        embedding = model.encode(text, convert_to_numpy=True)
        return embedding.tolist()
    else:
        # Fallback: random vector for testing
        import random
        return [random.uniform(-0.1, 0.1) for _ in range(384)]

def embed_batch(texts):
    """Generate embeddings for multiple texts."""
    model = get_model()
    if model:
        embeddings = model.encode(texts, convert_to_numpy=True)
        return [e.tolist() for e in embeddings]
    else:
        import random
        return [[random.uniform(-0.1, 0.1) for _ in range(384)] for _ in texts]

# C API wrappers
lib.cache_open.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
lib.cache_open.restype = ctypes.c_void_p

lib.cache_close.argtypes = [ctypes.c_void_p]
lib.cache_close.restype = None

lib.cache_set_vector.argtypes = [
    ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
    ctypes.POINTER(ctypes.c_float), ctypes.c_size_t, ctypes.c_uint64
]
lib.cache_set_vector.restype = ctypes.c_int

lib.cache_search_vector.argtypes = [
    ctypes.c_void_p, ctypes.POINTER(ctypes.c_float), ctypes.c_size_t,
    ctypes.c_int, ctypes.c_double, ctypes.c_void_p,
    ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_size_t)
]
lib.cache_search_vector.restype = ctypes.c_int

lib.cache_results_free.argtypes = [ctypes.c_void_p]
lib.cache_results_free.restype = None

class CacheResult(ctypes.Structure):
    _fields_ = [
        ("key", ctypes.c_char_p),
        ("value", ctypes.c_char_p),
        ("score", ctypes.c_double)
    ]

def store_with_embedding(db_dir, key, text, ttl_ms=0):
    """Store text with its vector embedding."""
    cache = lib.cache_open(db_dir.encode(), 100 * 1024 * 1024)
    if not cache:
        print(f"Error: Cannot open cache at {db_dir}")
        return False
    
    try:
        embedding = embed_text(text)
        vector_arr = (ctypes.c_float * len(embedding))(*embedding)
        
        # Store with value = text content
        ret = lib.cache_set_vector(
            cache, key.encode(), text.encode(),
            vector_arr, len(embedding), ttl_ms
        )
        
        if ret == 0:
            print(f"Stored: {key} (dim={len(embedding)})")
            return True
        else:
            print(f"Error storing {key}: {ret}")
            return False
    finally:
        lib.cache_close(cache)

def search_similar(db_dir, query_text, top_k=5, min_score=0.5):
    """Search for similar texts using vector similarity."""
    cache = lib.cache_open(db_dir.encode(), 100 * 1024 * 1024)
    if not cache:
        print(f"Error: Cannot open cache at {db_dir}")
        return
    
    try:
        embedding = embed_text(query_text)
        vector_arr = (ctypes.c_float * len(embedding))(*embedding)
        
        results_ptr = ctypes.c_void_p()
        count = ctypes.c_size_t()
        
        ret = lib.cache_search_vector(
            cache, vector_arr, len(embedding),
            top_k, min_score, None,
            ctypes.byref(results_ptr), ctypes.byref(count)
        )
        
        if ret != 0:
            print(f"Search error: {ret}")
            return
        
        if count.value == 0:
            print("No results found.")
            return
        
        # Cast results pointer to array
        results = ctypes.cast(results_ptr, ctypes.POINTER(CacheResult))
        
        print(f"\nQuery: '{query_text}'")
        print(f"Found {count.value} similar items:\n")
        
        for i in range(count.value):
            key = results[i].key.decode('utf-8') if results[i].key else "N/A"
            score = results[i].score
            print(f"  [{i+1}] {key} (score: {score:.3f})")
        
        lib.cache_results_free(results_ptr)
        
    finally:
        lib.cache_close(cache)

def main():
    if len(sys.argv) < 2:
        print("Usage:")
        print("  vector_helper.py embed 'text to embed'")
        print("  vector_helper.py store --db-dir DIR --key KEY 'text'")
        print("  vector_helper.py search --db-dir DIR 'query text'")
        sys.exit(1)
    
    cmd = sys.argv[1]
    
    if cmd == "embed":
        text = sys.argv[2] if len(sys.argv) > 2 else input("Enter text: ")
        embedding = embed_text(text)
        print(f"Embedding dimension: {len(embedding)}")
        print(f"First 5 values: {embedding[:5]}")
    
    elif cmd == "store":
        db_dir = "./vector_cache"
        key = None
        text = None
        
        i = 2
        while i < len(sys.argv):
            if sys.argv[i] == "--db-dir" and i + 1 < len(sys.argv):
                db_dir = sys.argv[i + 1]
                i += 2
            elif sys.argv[i] == "--key" and i + 1 < len(sys.argv):
                key = sys.argv[i + 1]
                i += 2
            else:
                text = sys.argv[i]
                i += 1
        
        if not key:
            key = f"doc_{hash(text) % 10000}"
        if not text:
            print("Error: No text provided")
            sys.exit(1)
        
        store_with_embedding(db_dir, key, text)
    
    elif cmd == "search":
        db_dir = "./vector_cache"
        query = None
        
        i = 2
        while i < len(sys.argv):
            if sys.argv[i] == "--db-dir" and i + 1 < len(sys.argv):
                db_dir = sys.argv[i + 1]
                i += 2
            else:
                query = sys.argv[i]
                i += 1
        
        if not query:
            print("Error: No query provided")
            sys.exit(1)
        
        search_similar(db_dir, query)
    
    else:
        print(f"Unknown command: {cmd}")
        sys.exit(1)

if __name__ == "__main__":
    main()
