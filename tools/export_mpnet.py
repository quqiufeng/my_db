#!/usr/bin/env python3
"""Download and export all-mpnet-base-v2 to ONNX"""
import os
import sys

model_dir = "models/all-mpnet-base-v2"
os.makedirs(model_dir, exist_ok=True)

print("Downloading all-mpnet-base-v2...")
from sentence_transformers import SentenceTransformer
model = SentenceTransformer('all-mpnet-base-v2')

print(f"Model dim: {model.get_sentence_embedding_dimension()}")

# Export to ONNX
print("Exporting to ONNX...")
try:
    from optimum.onnxruntime import ORTModelForFeatureExtraction
    from transformers import AutoTokenizer
    
    # Save model first
    model.save(model_dir)
    
    # Export using optimum
    tokenizer = AutoTokenizer.from_pretrained(model_dir)
    
    # Create dummy input
    dummy_input = "This is a test sentence for ONNX export."
    inputs = tokenizer(dummy_input, return_tensors="pt", padding="max_length", truncation=True, max_length=128)
    
    # Export
    import torch
    model_pt = model._first_module().auto_model
    model_pt.eval()
    
    torch.onnx.export(
        model_pt,
        (inputs['input_ids'], inputs['attention_mask']),
        os.path.join(model_dir, "model.onnx"),
        input_names=["input_ids", "attention_mask"],
        output_names=["token_embeddings"],
        dynamic_axes={
            "input_ids": {0: "batch_size", 1: "sequence"},
            "attention_mask": {0: "batch_size", 1: "sequence"},
            "token_embeddings": {0: "batch_size", 1: "sequence"}
        },
        opset_version=14,
    )
    print("ONNX export successful!")
    
    # Save tokenizer vocab
    vocab_file = os.path.join(model_dir, "vocab.txt")
    if hasattr(tokenizer, 'vocab'):
        with open(vocab_file, 'w', encoding='utf-8') as f:
            for token, idx in sorted(tokenizer.vocab.items(), key=lambda x: x[1]):
                f.write(f"{token}\n")
        print(f"Vocab saved: {len(tokenizer.vocab)} tokens")
    
except Exception as e:
    print(f"Export error: {e}")
    print("Trying alternative method...")
    
    # Alternative: use ONNX directly from sentence-transformers
    import torch
    from transformers import AutoModel, AutoTokenizer
    
    model_name = 'sentence-transformers/all-mpnet-base-v2'
    tokenizer = AutoTokenizer.from_pretrained(model_name)
    model_pt = AutoModel.from_pretrained(model_name)
    model_pt.eval()
    
    dummy_input = "test"
    inputs = tokenizer(dummy_input, return_tensors="pt", padding="max_length", truncation=True, max_length=128)
    
    with torch.no_grad():
        torch.onnx.export(
            model_pt,
            (inputs['input_ids'], inputs['attention_mask']),
            os.path.join(model_dir, "model.onnx"),
            input_names=["input_ids", "attention_mask"],
            output_names=["token_embeddings"],
            dynamic_axes={
                "input_ids": {0: "batch_size", 1: "sequence"},
                "attention_mask": {0: "batch_size", 1: "sequence"},
                "token_embeddings": {0: "batch_size", 1: "sequence"}
            },
            opset_version=14,
        )
    
    # Save vocab
    vocab_file = os.path.join(model_dir, "vocab.txt")
    if hasattr(tokenizer, 'vocab'):
        with open(vocab_file, 'w', encoding='utf-8') as f:
            for token, idx in sorted(tokenizer.vocab.items(), key=lambda x: x[1]):
                f.write(f"{token}\n")
        print(f"Vocab saved: {len(tokenizer.vocab)} tokens")

print(f"\nModel files in {model_dir}:")
for f in os.listdir(model_dir):
    size = os.path.getsize(os.path.join(model_dir, f))
    print(f"  {f}: {size / 1024 / 1024:.1f} MB")
