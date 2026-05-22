#!/usr/bin/env python3
"""Encode query text to vector using Jina model"""
import sys
import numpy as np
from tokenizers import Tokenizer
import onnxruntime as ort

model_path = "/opt/models/jina-embeddings-v2-base-code/model.onnx"
tokenizer_path = "/opt/models/jina-embeddings-v2-base-code/tokenizer.json"

tokenizer = Tokenizer.from_file(tokenizer_path)
sess = ort.InferenceSession(model_path, providers=['CUDAExecutionProvider', 'CPUExecutionProvider'])

# Read query from stdin
query = sys.stdin.read().strip()

encoded = tokenizer.encode(query)
ids = encoded.ids + [1] * (512 - len(encoded.ids))
mask = [1] * len(encoded.ids) + [0] * (512 - len(encoded.ids))

out = sess.run(None, {
    'input_ids': np.array([ids], dtype=np.int64),
    'attention_mask': np.array([mask], dtype=np.int64),
})

valid_len = sum(mask)
vec = out[0][0, :valid_len].mean(axis=0)
norm = np.linalg.norm(vec)
if norm > 1e-6:
    vec = vec / norm

# Output as space-separated floats
print(' '.join(f'{v:.6f}' for v in vec))
