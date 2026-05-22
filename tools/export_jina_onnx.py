#!/usr/bin/env python3
"""
Export Jina Embeddings v2 base code to ONNX and create vocab.txt for C tokenizer
"""

import os
import json
import torch
from transformers import BertTokenizer, BertModel, BertConfig

INPUT_DIR = "/opt/jina-embeddings-v2-base-code"
OUTPUT_DIR = "/opt/models/jina-embeddings-v2-base-code"

def export():
    print("Loading Jina model from /opt/jina-embeddings-v2-base-code...")
    
    # Load config
    with open(os.path.join(INPUT_DIR, "config.json"), "r") as f:
        config_dict = json.load(f)
    
    # Create standard BERT config
    config = BertConfig(
        vocab_size=config_dict["vocab_size"],
        hidden_size=config_dict["hidden_size"],
        num_hidden_layers=config_dict["num_hidden_layers"],
        num_attention_heads=config_dict["num_attention_heads"],
        intermediate_size=config_dict["intermediate_size"],
        max_position_embeddings=config_dict["max_position_embeddings"],
        type_vocab_size=config_dict.get("type_vocab_size", 2),
        layer_norm_eps=config_dict.get("layer_norm_eps", 1e-12),
        hidden_dropout_prob=0.0,
        attention_probs_dropout_prob=0.0,
    )
    
    # Create model and load weights
    model = BertModel(config)
    state_dict = torch.load(os.path.join(INPUT_DIR, "pytorch_model.bin"), map_location="cpu")
    model.load_state_dict(state_dict, strict=False)
    model.eval()
    
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    
    # Create vocab.txt from tokenizer.json
    vocab_txt_path = os.path.join(OUTPUT_DIR, "vocab.txt")
    print(f"Creating vocab.txt...")
    
    # Load tokenizer to get vocab
    from tokenizers import Tokenizer
    tokenizer = Tokenizer.from_file(os.path.join(INPUT_DIR, "tokenizer.json"))
    vocab = tokenizer.get_vocab()
    
    # Sort by ID and write
    sorted_vocab = sorted(vocab.items(), key=lambda x: x[1])
    with open(vocab_txt_path, "w", encoding="utf-8") as f:
        for token, idx in sorted_vocab:
            f.write(f"{token}\n")
    
    print(f"  Vocab size: {len(sorted_vocab)}")
    
    # Export ONNX
    model_path = os.path.join(OUTPUT_DIR, "model.onnx")
    print(f"Exporting ONNX to {model_path}...")
    
    dummy_input_ids = torch.ones(1, 512, dtype=torch.long)
    dummy_attention_mask = torch.ones(1, 512, dtype=torch.long)
    dummy_token_type_ids = torch.zeros(1, 512, dtype=torch.long)
    
    torch.onnx.export(
        model,
        (dummy_input_ids, dummy_attention_mask, dummy_token_type_ids),
        model_path,
        input_names=["input_ids", "attention_mask", "token_type_ids"],
        output_names=["last_hidden_state", "pooler_output"],
        dynamic_axes={
            "input_ids": {0: "batch_size", 1: "sequence_length"},
            "attention_mask": {0: "batch_size", 1: "sequence_length"},
            "token_type_ids": {0: "batch_size", 1: "sequence_length"},
            "last_hidden_state": {0: "batch_size", 1: "sequence_length"},
            "pooler_output": {0: "batch_size"},
        },
        opset_version=14,
        do_constant_folding=True,
    )
    
    # Save config
    config_out = {
        "model_type": "jina",
        "base_model": "bert",
        "max_seq_length": 512,  # Use 512 for now (C tokenizer limit)
        "dim": config_dict["hidden_size"],
        "vocab_size": len(sorted_vocab),
        "pad_token_id": 0,
        "cls_token_id": 0,
        "sep_token_id": 2,
        "unk_token_id": 1,
    }
    
    config_path = os.path.join(OUTPUT_DIR, "config.json")
    with open(config_path, "w") as f:
        json.dump(config_out, f, indent=2)
    
    print(f"\nExport complete!")
    print(f"  Model: {model_path}")
    print(f"  Vocab: {vocab_txt_path}")
    print(f"  Config: {config_path}")
    print(f"  Dim: {config_dict['hidden_size']}, Max seq: 512")
    
    # Test
    print("\nTesting tokenization...")
    text = "sd_image_t* txt2img(const char* prompt)"
    encoded = tokenizer.encode(text)
    print(f"  Input: {text}")
    print(f"  Tokens: {encoded.tokens}")
    
    return config_out

if __name__ == "__main__":
    export()
