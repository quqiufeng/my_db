#!/usr/bin/env python3
"""Test semantic search on embedded ebooks."""

import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from sentence_transformers import SentenceTransformer
import ctypes

lib_path = os.path.join(os.path.dirname(__file__), '..', 'libmydb.so')
lib = ctypes.CDLL(lib_path)

lib.cache_open.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
lib.cache_open.restype = ctypes.c_void_p
lib.cache_close.argtypes = [ctypes.c_void_p]
lib.cache_close.restype = None
lib.cache_search_vector.argtypes = [
    ctypes.c_void_p, ctypes.POINTER(ctypes.c_float), ctypes.c_size_t,
    ctypes.c_int, ctypes.c_double, ctypes.c_void_p,
    ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_size_t)
]
lib.cache_search_vector.restype = ctypes.c_int
lib.cache_results_free.argtypes = [ctypes.c_void_p]
lib.cache_results_free.restype = None

class CacheResult(ctypes.Structure):
    _fields_ = [("key", ctypes.c_char_p), ("value", ctypes.c_char_p), ("score", ctypes.c_double)]

def semantic_search(cache_dir, query, top_k=5):
    cache = lib.cache_open(cache_dir.encode(), 0)
    if not cache:
        print(f"Failed to open cache: {cache_dir}")
        return
    
    try:
        # Generate query embedding
        model = SentenceTransformer('all-MiniLM-L6-v2')
        embedding = model.encode(query, convert_to_numpy=True)
        vector_arr = (ctypes.c_float * len(embedding))(*embedding.tolist())
        
        results_ptr = ctypes.c_void_p()
        count = ctypes.c_size_t()
        
        ret = lib.cache_search_vector(
            cache, vector_arr, len(embedding),
            top_k, 0.3, None,
            ctypes.byref(results_ptr), ctypes.byref(count)
        )
        
        if ret != 0:
            print(f"Search error: {ret}")
            return
        
        if count.value == 0:
            print("No results found.")
            return
        
        results = ctypes.cast(results_ptr, ctypes.POINTER(CacheResult))
        
        print(f"\nQuery: '{query}'")
        print(f"Found {count.value} results:\n")
        
        for i in range(min(count.value, top_k)):
            key = results[i].key.decode('utf-8') if results[i].key else "N/A"
            value = results[i].value.decode('utf-8') if results[i].value else "N/A"
            score = results[i].score
            # Extract paragraph text from JSON
            import json
            try:
                data = json.loads(value)
                text = data.get('c', value[:100])
            except:
                text = value[:100]
            print(f"[{i+1}] {key}")
            print(f"    Score: {score:.4f}")
            print(f"    Text: {text[:120]}...\n")
        
        lib.cache_results_free(results_ptr)
        
    finally:
        lib.cache_close(cache)

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python3 test_semantic_search.py <query>")
        sys.exit(1)
    
    query = sys.argv[1]
    semantic_search("/tmp/test_embed_cache", query)
