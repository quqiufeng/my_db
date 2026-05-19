#!/usr/bin/env python3
"""
导出 CodeBERT 模型为 ONNX 格式
用于代码语义搜索
"""

import os
import torch
from transformers import AutoTokenizer, AutoModel
import onnx

MODEL_NAME = "microsoft/codebert-base"
OUTPUT_DIR = "models/codebert-base"

def export_codebert():
    print(f"Loading {MODEL_NAME}...")
    tokenizer = AutoTokenizer.from_pretrained(MODEL_NAME)
    model = AutoModel.from_pretrained(MODEL_NAME)
    model.eval()
    
    # Create output directory
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    
    # Export tokenizer vocab
    vocab_file = os.path.join(OUTPUT_DIR, "vocab.txt")
    print(f"Saving vocab to {vocab_file}...")
    with open(vocab_file, "w", encoding="utf-8") as f:
        vocab = tokenizer.get_vocab()
        # Sort by token ID
        sorted_vocab = sorted(vocab.items(), key=lambda x: x[1])
        for token, idx in sorted_vocab:
            f.write(f"{token}\n")
    print(f"  Vocab size: {len(vocab)}")
    
    # Export ONNX model
    model_path = os.path.join(OUTPUT_DIR, "model.onnx")
    print(f"Exporting ONNX model to {model_path}...")
    
    # Dummy input
    dummy_input_ids = torch.ones(1, 128, dtype=torch.long)
    dummy_attention_mask = torch.ones(1, 128, dtype=torch.long)
    
    # Export
    torch.onnx.export(
        model,
        (dummy_input_ids, dummy_attention_mask),
        model_path,
        input_names=["input_ids", "attention_mask"],
        output_names=["last_hidden_state"],
        dynamic_axes={
            "input_ids": {0: "batch_size", 1: "sequence_length"},
            "attention_mask": {0: "batch_size", 1: "sequence_length"},
            "last_hidden_state": {0: "batch_size", 1: "sequence_length"}
        },
        opset_version=14,
        do_constant_folding=True
    )
    
    # Save config
    config = {
        "model_type": "codebert",
        "max_seq_length": 512,  # CodeBERT supports 512
        "dim": 768,
        "vocab_size": len(vocab),
        "pad_token_id": tokenizer.pad_token_id,
        "cls_token_id": tokenizer.cls_token_id,
        "sep_token_id": tokenizer.sep_token_id,
        "unk_token_id": tokenizer.unk_token_id,
    }
    
    import json
    config_file = os.path.join(OUTPUT_DIR, "config.json")
    with open(config_file, "w") as f:
        json.dump(config, f, indent=2)
    
    print(f"\nExport complete!")
    print(f"  Model: {model_path}")
    print(f"  Vocab: {vocab_file}")
    print(f"  Config: {config_file}")
    print(f"  Dim: 768")
    print(f"  Max seq length: 512")
    
    return config

if __name__ == "__main__":
    export_codebert()
