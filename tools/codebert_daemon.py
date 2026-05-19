#!/usr/bin/env python3
"""
CodeBERT encoding daemon - persistent process for fast batch encoding
Communicates via stdin/stdout JSON lines protocol

Protocol:
  Input:  {"cmd": "encode", "items": [{"name": "...", "text": "..."}, ...]}
  Output: {"results": [{"name": "...", "vector": [...]}, ...]}
  
  Input:  {"cmd": "query", "text": "..."}
  Output: {"vector": [...]}
  
  Input:  {"cmd": "quit"}
  Output: {"status": "ok"}
"""

import sys
import json
import numpy as np
import onnxruntime as ort
from transformers import BertTokenizer

def main():
    model_dir = sys.argv[1] if len(sys.argv) > 1 else "models/codebert-base"
    
    print(f"Loading CodeBERT from {model_dir}...", file=sys.stderr)
    tokenizer = BertTokenizer.from_pretrained(model_dir)
    session = ort.InferenceSession(f"{model_dir}/model.onnx")
    dim = 768
    max_seq = 512
    print("Ready", file=sys.stderr, flush=True)
    
    while True:
        try:
            line = sys.stdin.readline()
            if not line:
                break
            
            req = json.loads(line)
            cmd = req.get("cmd")
            
            if cmd == "quit":
                print(json.dumps({"status": "ok"}), flush=True)
                break
            
            elif cmd == "query":
                text = req["text"]
                inputs = tokenizer(text, max_length=max_seq, padding='max_length', 
                                  truncation=True, return_tensors='np')
                outputs = session.run(None, {
                    'input_ids': inputs['input_ids'].astype(np.int64),
                    'attention_mask': inputs['attention_mask'].astype(np.int64)
                })
                vec = mean_pool(outputs[0], inputs['attention_mask'].astype(np.float32))[0]
                print(json.dumps({"vector": vec.tolist()}), flush=True)
            
            elif cmd == "encode":
                items = req["items"]
                texts = [item["text"] for item in items]
                names = [item.get("name", "") for item in items]
                
                inputs = tokenizer(texts, max_length=max_seq, padding='max_length',
                                  truncation=True, return_tensors='np')
                outputs = session.run(None, {
                    'input_ids': inputs['input_ids'].astype(np.int64),
                    'attention_mask': inputs['attention_mask'].astype(np.int64)
                })
                vectors = mean_pool(outputs[0], inputs['attention_mask'].astype(np.float32))
                
                results = []
                for name, vec in zip(names, vectors):
                    results.append({"name": name, "vector": vec.tolist()})
                
                print(json.dumps({"results": results}), flush=True)
                
        except json.JSONDecodeError:
            continue
        except Exception as e:
            print(json.dumps({"error": str(e)}), flush=True)
            continue

def mean_pool(last_hidden, attention_mask):
    mask = np.expand_dims(attention_mask, axis=-1)
    sum_emb = np.sum(last_hidden * mask, axis=1)
    sum_mask = np.sum(mask, axis=1)
    pooled = sum_emb / sum_mask
    norms = np.linalg.norm(pooled, axis=1, keepdims=True)
    norms[norms == 0] = 1
    return pooled / norms

if __name__ == '__main__':
    main()
