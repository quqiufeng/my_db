#!/usr/bin/env python3
"""
批量向量生成器 - Python 协调 + C++ 多线程编码

用法:
    python3 tools/batch_vector_generator.py /path/to/cache /code/repo/namespace
"""

import sys
import os
import json
import subprocess
import tempfile
import struct

sys.path.insert(0, '/home/dministrator/my_db')
from mydb.cache import open_cache

def export_texts(cache_dir: str, repo: str) -> tuple:
    """导出所有符号文本到临时二进制文件，返回 (文件路径, 数量)"""
    # 使用大内存限制
    cache = open_cache(cache_dir, 10*1024*1024*1024)  # 10GB
    
    items = []
    prefix = f"{repo}/_text/"
    
    for key, value in cache.items():
        if key.startswith(prefix):
            try:
                data = json.loads(value)
                func_name = key[len(prefix):]
                text = data.get('text', '')
                if text.strip():
                    items.append({
                        'name': func_name,
                        'text': text,
                        'file': data.get('file', ''),
                        'line': data.get('line', 0),
                    })
            except:
                pass
    
    cache.close()
    
    # 写入临时二进制文件
    fd, path = tempfile.mkstemp(suffix='.bin', prefix='vectors_in_')
    
    with os.fdopen(fd, 'wb') as f:
        f.write(struct.pack('I', len(items)))
        
        for item in items:
            name_bytes = item['name'].encode('utf-8')
            text_bytes = item['text'].encode('utf-8')
            file_bytes = item['file'].encode('utf-8')
            
            f.write(struct.pack('I', len(name_bytes)))
            f.write(name_bytes)
            f.write(struct.pack('I', len(text_bytes)))
            f.write(text_bytes)
            f.write(struct.pack('I', len(file_bytes)))
            f.write(file_bytes)
            f.write(struct.pack('i', item['line']))
    
    return path, len(items)

def run_cpp_encoder(input_bin: str, output_bin: str, num_threads: int = 0) -> bool:
    """运行 C++ 编码器"""
    tool = os.path.join(os.path.dirname(__file__), 'vector_indexer')
    
    if not os.path.exists(tool):
        print(f"C++ encoder not found: {tool}")
        print("Building...")
        
        build_script = os.path.join(os.path.dirname(__file__), 'build_vector_indexer.py')
        if os.path.exists(build_script):
            result = subprocess.run([sys.executable, build_script], capture_output=True, text=True)
            if result.returncode != 0:
                print(f"Build failed:\n{result.stderr}")
                return False
        else:
            print("Build script not found")
            return False
    
    cmd = [tool, input_bin, output_bin]
    
    if num_threads > 0:
        cmd.extend(['--threads', str(num_threads)])
    
    print(f"\nRunning C++ encoder: {' '.join(cmd)}")
    
    result = subprocess.run(cmd, capture_output=True, text=True)
    
    if result.returncode != 0:
        print(f"C++ encoder failed:\n{result.stderr}")
        return False
    
    print(result.stdout)
    return True

def import_vectors(cache_dir: str, repo: str, vector_bin: str) -> int:
    """将向量文件导入 cache"""
    # 使用大内存限制
    cache = open_cache(cache_dir, 10*1024*1024*1024)  # 10GB
    
    count = 0
    dim = 384
    
    try:
        with open(vector_bin, 'rb') as f:
            while True:
                name_len_bytes = f.read(4)
                if len(name_len_bytes) < 4:
                    break
                
                name_len = struct.unpack('I', name_len_bytes)[0]
                if name_len > 1024:
                    print(f"Warning: invalid name length {name_len}")
                    break
                
                name = f.read(name_len).decode('utf-8')
                
                vector_data = f.read(dim * 4)
                if len(vector_data) < dim * 4:
                    print(f"Warning: incomplete vector for {name}")
                    break
                
                vector = struct.unpack(f'{dim}f', vector_data)
                
                key = f"{repo}/vectors/{name}"
                value = json.dumps({
                    'name': name,
                    'vector': list(vector),
                })
                
                cache.set(key, value, 0)
                count += 1
    except Exception as e:
        print(f"Error importing: {e}")
    
    cache.close()
    return count

def main():
    import argparse
    
    parser = argparse.ArgumentParser(description='Batch vector generator (C++ multi-threaded)')
    parser.add_argument('cache_dir', help='Cache directory')
    parser.add_argument('repo', help='Repository namespace')
    parser.add_argument('--threads', '-t', type=int, default=0, help='Number of threads (0=auto)')
    
    args = parser.parse_args()
    
    print("="*60)
    print("Batch Vector Generator (C++ Multi-threaded)")
    print("="*60)
    
    # 1. 导出文本
    print("\nStep 1: Exporting texts from cache...")
    input_bin, count = export_texts(args.cache_dir, args.repo)
    print(f"  Exported {count} symbols")
    
    # 2. C++ 编码
    print("\nStep 2: Running C++ encoder...")
    output_bin = input_bin.replace('.bin', '_out.bin')
    
    if not run_cpp_encoder(input_bin, output_bin, args.threads):
        os.unlink(input_bin)
        sys.exit(1)
    
    # 3. 导入向量
    print("\nStep 3: Importing vectors to cache...")
    imported = import_vectors(args.cache_dir, args.repo, output_bin)
    print(f"  Imported {imported} vectors")
    
    # 清理
    os.unlink(input_bin)
    os.unlink(output_bin)
    
    print("\n" + "="*60)
    print("Done!")
    print("="*60)

if __name__ == '__main__':
    main()
