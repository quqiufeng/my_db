#!/usr/bin/env python3
"""
CodeBERT encoding script - called by C tools for tokenization + ONNX inference

Usage: python3 codebert_encode.py --model-dir models/codebert-base --batch
Reads: JSON lines from stdin: {"text": "code snippet", "name": "func_name"}
Writes: JSON lines to stdout: {"name": "func_name", "vector": [0.1, 0.2, ...]}

Or single query mode:
Usage: python3 codebert_encode.py --model-dir models/codebert-base --query "text to encode"
Writes: JSON: {"vector": [0.1, 0.2, ...]}
"""

import sys
import json
import numpy as np
import onnxruntime as ort
from transformers import BertTokenizer
import argparse

class CodeBertEncoder:
    def __init__(self, model_dir):
        self.model_dir = model_dir
        self.tokenizer = BertTokenizer.from_pretrained(model_dir)
        
        # Load ONNX model
        model_path = f"{model_dir}/model.onnx"
        self.session = ort.InferenceSession(model_path)
        
        # Get model info
        self.dim = 768
        self.max_seq_length = 512
        
    def encode(self, text):
        """Encode single text to vector"""
        # Tokenize
        inputs = self.tokenizer(
            text,
            max_length=self.max_seq_length,
            padding='max_length',
            truncation=True,
            return_tensors='np'
        )
        
        # Run inference
        input_ids = inputs['input_ids'].astype(np.int64)
        attention_mask = inputs['attention_mask'].astype(np.int64)
        
        outputs = self.session.run(
            None,
            {
                'input_ids': input_ids,
                'attention_mask': attention_mask
            }
        )
        
        # Mean pooling (RoBERTa doesn't have pooler)
        last_hidden = outputs[0]  # [1, seq_len, 768]
        mask = attention_mask.astype(np.float32)
        mask = np.expand_dims(mask, axis=-1)  # [1, seq_len, 1]
        
        # Masked mean
        sum_embeddings = np.sum(last_hidden * mask, axis=1)
        sum_mask = np.sum(mask, axis=1)
        mean_pooled = sum_embeddings / sum_mask
        
        # L2 normalize
        norm = np.linalg.norm(mean_pooled, axis=1, keepdims=True)
        if norm > 0:
            mean_pooled = mean_pooled / norm
        
        return mean_pooled[0].tolist()
    
    def encode_batch(self, texts):
        """Encode batch of texts"""
        if not texts:
            return []
        
        # Tokenize batch
        inputs = self.tokenizer(
            texts,
            max_length=self.max_seq_length,
            padding='max_length',
            truncation=True,
            return_tensors='np'
        )
        
        input_ids = inputs['input_ids'].astype(np.int64)
        attention_mask = inputs['attention_mask'].astype(np.int64)
        
        outputs = self.session.run(
            None,
            {
                'input_ids': input_ids,
                'attention_mask': attention_mask
            }
        )
        
        # Mean pooling
        last_hidden = outputs[0]  # [batch, seq_len, 768]
        mask = attention_mask.astype(np.float32)
        mask = np.expand_dims(mask, axis=-1)
        
        sum_embeddings = np.sum(last_hidden * mask, axis=1)
        sum_mask = np.sum(mask, axis=1)
        mean_pooled = sum_embeddings / sum_mask
        
        # L2 normalize
        norms = np.linalg.norm(mean_pooled, axis=1, keepdims=True)
        norms[norms == 0] = 1  # avoid division by zero
        mean_pooled = mean_pooled / norms
        
        return mean_pooled.tolist()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--model-dir', default='models/codebert-base', help='Model directory')
    parser.add_argument('--batch', action='store_true', help='Batch mode (read JSON lines from stdin)')
    parser.add_argument('--query', help='Single query mode')
    parser.add_argument('--dim', type=int, default=768, help='Output dimension')
    args = parser.parse_args()
    
    encoder = CodeBertEncoder(args.model_dir)
    
    if args.query:
        # Single query mode
        vector = encoder.encode(args.query)
        print(json.dumps({"vector": vector}))
    
    elif args.batch:
        # Batch mode
        batch = []
        batch_names = []
        BATCH_SIZE = 32  # Process in smaller batches to avoid memory issues
        
        for line in sys.stdin:
            line = line.strip()
            if not line:
                continue
            try:
                data = json.loads(line)
                batch.append(data['text'])
                batch_names.append(data.get('name', ''))
                
                if len(batch) >= BATCH_SIZE:
                    vectors = encoder.encode_batch(batch)
                    for name, vec in zip(batch_names, vectors):
                        print(json.dumps({"name": name, "vector": vec}))
                        sys.stdout.flush()
                    batch = []
                    batch_names = []
            except json.JSONDecodeError:
                continue
        
        # Process remaining
        if batch:
            vectors = encoder.encode_batch(batch)
            for name, vec in zip(batch_names, vectors):
                print(json.dumps({"name": name, "vector": vec}))
    
    else:
        print("Usage: codebert_encode.py --batch | --query 'text'", file=sys.stderr)
        sys.exit(1)

if __name__ == '__main__':
    main()
