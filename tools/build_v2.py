#!/usr/bin/env python3
"""编译 V2 版本"""
import subprocess
import os
import sys

def build():
    src_dir = os.path.dirname(os.path.abspath(__file__))
    cpp_file = os.path.join(src_dir, "vector_indexer_v2.cpp")
    output = os.path.join(src_dir, "vector_indexer_v2")
    
    if not os.path.exists(cpp_file):
        print(f"Source file not found: {cpp_file}")
        return False
    
    onnx_path = "/opt/piper-src/build/p/src/piper_phonemize_external/lib/onnxruntime-linux-x64-1.14.1"
    root_dir = os.path.dirname(src_dir)
    
    cmd = [
        "g++", "-std=c++17", "-O3", "-fopenmp",
        "-o", output,
        cpp_file,
        "-I", f"{onnx_path}/include",
        "-I", os.path.join(root_dir, "include"),
        "-L", f"{onnx_path}/lib",
        "-L", root_dir,
        "-lonnxruntime",
        "-lonnx_embedder",
        "-lpthread",
        "-Wl,-rpath", f"{onnx_path}/lib",
        "-Wl,-rpath", root_dir,
    ]
    
    print(f"Compiling V2...")
    result = subprocess.run(cmd, capture_output=True, text=True)
    
    if result.returncode != 0:
        print(f"Failed:\n{result.stderr}")
        return False
    
    print(f"Success: {output}")
    return True

if __name__ == '__main__':
    if build():
        sys.exit(0)
    else:
        sys.exit(1)
