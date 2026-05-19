#!/usr/bin/env python3
"""Export all-mpnet-base-v2 to ONNX"""
import os
import sys
import torch
from transformers import AutoModel, AutoTokenizer

src_dir = "/opt/all-mpnet-base-v2"
dst_dir = "models/all-mpnet-base-v2"
os.makedirs(dst_dir, exist_ok=True)

print("Loading model...")
model = AutoModel.from_pretrained(src_dir)
tokenizer = AutoTokenizer.from_pretrained(src_dir)
model.eval()

print(f"Model dim: {model.config.hidden_size}")

# Export to ONNX
dummy_text = "test sentence for export"
inputs = tokenizer(dummy_text, return_tensors="pt", padding="max_length", truncation=True, max_length=128)

onnx_path = os.path.join(dst_dir, "model.onnx")
print(f"Exporting to {onnx_path}...")

torch.onnx.export(
    model,
    (inputs['input_ids'], inputs['attention_mask']),
    onnx_path,
    input_names=["input_ids", "attention_mask"],
    output_names=["token_embeddings"],
    dynamic_axes={
        "input_ids": {0: "batch_size", 1: "sequence"},
        "attention_mask": {0: "batch_size", 1: "sequence"},
        "token_embeddings": {0: "batch_size", 1: "sequence"}
    },
    opset_version=14,
)

print(f"ONNX exported: {os.path.getsize(onnx_path) / 1024 / 1024:.1f} MB")

# Save vocab
vocab_file = os.path.join(dst_dir, "vocab.txt")
if hasattr(tokenizer, 'vocab'):
    with open(vocab_file, 'w', encoding='utf-8') as f:
        for token, idx in sorted(tokenizer.vocab.items(), key=lambda x: x[1]):
            f.write(f"{token}\n")
    print(f"Vocab saved: {len(tokenizer.vocab)} tokens")
elif hasattr(tokenizer, 'get_vocab'):
    vocab = tokenizer.get_vocab()
    with open(vocab_file, 'w', encoding='utf-8') as f:
        for token, idx in sorted(vocab.items(), key=lambda x: x[1]):
            f.write(f"{token}\n")
    print(f"Vocab saved: {len(vocab)} tokens")

print(f"\nDone! Files in {dst_dir}:")
for f in os.listdir(dst_dir):
    size = os.path.getsize(os.path.join(dst_dir, f))
    print(f"  {f}: {size / 1024 / 1024:.1f} MB")
