#!/usr/bin/env python3
"""
Export sentence-transformers model to ONNX format for C inference.

Usage:
    python3 tools/export_onnx.py
    
Outputs:
    models/all-MiniLM-L6-v2/model.onnx
    models/all-MiniLM-L6-v2/vocab.txt
"""

import os
import sys
import json
from pathlib import Path

try:
    from sentence_transformers import SentenceTransformer
    import torch
    import torch.onnx
    ST_AVAILABLE = True
except ImportError:
    ST_AVAILABLE = False
    print("Error: sentence-transformers and torch required")
    sys.exit(1)

# Model configuration
MODEL_NAME = 'all-MiniLM-L6-v2'
OUTPUT_DIR = Path('models') / MODEL_NAME
MAX_SEQ_LENGTH = 128
HIDDEN_SIZE = 384  # all-MiniLM-L6-v2 output dimension

def export_model():
    print(f"Loading {MODEL_NAME}...")
    model = SentenceTransformer(MODEL_NAME)
    model.eval()
    
    # Get the underlying transformer model
    transformer = model[0].auto_model
    transformer = transformer.to('cpu')
    tokenizer = model[0].tokenizer
    
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    
    # Export tokenizer vocab
    vocab_file = OUTPUT_DIR / 'vocab.txt'
    with open(vocab_file, 'w', encoding='utf-8') as f:
        for token, idx in sorted(tokenizer.vocab.items(), key=lambda x: x[1]):
            f.write(token + '\n')
    print(f"Exported vocab: {vocab_file} ({len(tokenizer.vocab)} tokens)")
    
    # Export config
    config = {
        'model_type': 'bert',
        'max_seq_length': MAX_SEQ_LENGTH,
        'hidden_size': HIDDEN_SIZE,
        'num_attention_heads': transformer.config.num_attention_heads,
        'num_hidden_layers': transformer.config.num_hidden_layers,
        'pad_token_id': tokenizer.pad_token_id,
        'cls_token_id': tokenizer.cls_token_id,
        'sep_token_id': tokenizer.sep_token_id,
        'unk_token_id': tokenizer.unk_token_id,
    }
    config_file = OUTPUT_DIR / 'config.json'
    with open(config_file, 'w') as f:
        json.dump(config, f, indent=2)
    print(f"Exported config: {config_file}")
    
    # Export ONNX model (base transformer only, mean pooling in C)
    onnx_path = OUTPUT_DIR / 'model.onnx'
    
    # Create dummy inputs
    dummy_input_ids = torch.zeros(1, MAX_SEQ_LENGTH, dtype=torch.long)
    dummy_attention_mask = torch.ones(1, MAX_SEQ_LENGTH, dtype=torch.long)
    
    # Export
    print(f"Exporting ONNX model to {onnx_path}...")
    
    with torch.no_grad():
        torch.onnx.export(
            transformer,
            (dummy_input_ids, dummy_attention_mask),
            str(onnx_path),
            input_names=['input_ids', 'attention_mask'],
            output_names=['token_embeddings'],
            dynamic_axes={
                'input_ids': {0: 'batch_size', 1: 'sequence_length'},
                'attention_mask': {0: 'batch_size', 1: 'sequence_length'},
                'token_embeddings': {0: 'batch_size', 1: 'sequence_length'}
            },
            opset_version=14,
            do_constant_folding=True,
        )
    
    print(f"ONNX model exported: {onnx_path}")
    print(f"  Input:  input_ids[batch, seq], attention_mask[batch, seq]")
    print(f"  Output: token_embeddings[batch, seq, {HIDDEN_SIZE}]")
    print(f"\nMean pooling and L2 normalization will be done in C code.")
    print(f"\nDone! Files in {OUTPUT_DIR}:")
    for f in OUTPUT_DIR.iterdir():
        print(f"  - {f.name}")

if __name__ == '__main__':
    export_model()
