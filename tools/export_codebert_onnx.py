#!/usr/bin/env python3
"""
Export CodeBERT (RoBERTa) to ONNX and create vocab.txt for C tokenizer
"""

import os
import json
import torch
from transformers import AutoTokenizer, AutoModel

INPUT_DIR = "/opt/codebert"
OUTPUT_DIR = "/home/dministrator/my_db/models/codebert-base"

def export():
    print("Loading CodeBERT from /opt/codebert...")
    
    # Load model and tokenizer
    tokenizer = AutoTokenizer.from_pretrained(INPUT_DIR)
    model = AutoModel.from_pretrained(INPUT_DIR)
    model.eval()
    
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    
    # Create vocab.txt from vocab.json
    vocab_json_path = os.path.join(INPUT_DIR, "vocab.json")
    vocab_txt_path = os.path.join(OUTPUT_DIR, "vocab.txt")
    
    print(f"Converting vocab.json to vocab.txt...")
    with open(vocab_json_path, "r", encoding="utf-8") as f:
        vocab_dict = json.load(f)
    
    # Sort by ID
    sorted_vocab = sorted(vocab_dict.items(), key=lambda x: x[1])
    with open(vocab_txt_path, "w", encoding="utf-8") as f:
        for token, idx in sorted_vocab:
            # BPE tokens start with Ġ for space-prefixed, keep as-is
            f.write(f"{token}\n")
    
    print(f"  Vocab size: {len(sorted_vocab)}")
    
    # Export ONNX
    model_path = os.path.join(OUTPUT_DIR, "model.onnx")
    print(f"Exporting ONNX to {model_path}...")
    
    dummy_input_ids = torch.ones(1, 512, dtype=torch.long)
    dummy_attention_mask = torch.ones(1, 512, dtype=torch.long)
    
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
        "base_model": "roberta",
        "max_seq_length": 512,
        "dim": 768,
        "vocab_size": len(sorted_vocab),
        "pad_token_id": tokenizer.pad_token_id,
        "cls_token_id": tokenizer.cls_token_id,  # RoBERTa: <s>
        "sep_token_id": tokenizer.sep_token_id,  # RoBERTa: </s>
        "unk_token_id": tokenizer.unk_token_id,
        "bos_token_id": 0,
        "eos_token_id": 2,
    }
    
    config_path = os.path.join(OUTPUT_DIR, "config.json")
    with open(config_path, "w") as f:
        json.dump(config, f, indent=2)
    
    print(f"\nExport complete!")
    print(f"  Model: {model_path}")
    print(f"  Vocab: {vocab_txt_path}")
    print(f"  Config: {config_path}")
    print(f"  Dim: 768, Max seq: 512")
    
    # Test
    print("\nTesting tokenization...")
    text = "sd_image_t* txt2img(const char* prompt)"
    tokens = tokenizer.tokenize(text)
    print(f"  Input: {text}")
    print(f"  Tokens: {tokens}")
    
    return config

if __name__ == "__main__":
    export()
