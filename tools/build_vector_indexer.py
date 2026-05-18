#!/usr/bin/env python3
"""
C++ 向量生成器编译脚本

用法:
    python3 tools/build_vector_indexer.py
    
编译产物:
    tools/vector_indexer    # 可执行文件
"""

import subprocess
import os
import sys

def build():
    """编译 C++ 向量生成器"""
    
    # 源文件
    src_dir = os.path.dirname(os.path.abspath(__file__))
    cpp_file = os.path.join(src_dir, "vector_indexer.cpp")
    
    # 输出文件
    output = os.path.join(src_dir, "vector_indexer")
    
    # 检查源文件
    if not os.path.exists(cpp_file):
        print(f"Source file not found: {cpp_file}")
        return False
    
    # ONNX Runtime 路径
    onnx_path = "/opt/piper-src/build/p/src/piper_phonemize_external/lib/onnxruntime-linux-x64-1.14.1"
    
    # 编译命令
    # 项目根目录
    root_dir = os.path.dirname(src_dir)
    
    cmd = [
        "g++", "-std=c++17", "-O3", "-fopenmp",  # OpenMP 多线程
        "-o", output,
        cpp_file,
        "-I", f"{onnx_path}/include",
        "-I", os.path.join(root_dir, "include"),
        "-L", f"{onnx_path}/lib",
        "-L", root_dir,  # libonnx_embedder.so 所在目录
        "-lonnxruntime",
        "-lonnx_embedder",
        "-lpthread",  # 线程支持
        "-Wl,-rpath", f"{onnx_path}/lib",  # 运行时库路径
        "-Wl,-rpath", root_dir,  # libonnx_embedder.so 运行时路径
    ]
    
    print(f"Compiling C++ vector indexer...")
    print(f"Command: {' '.join(cmd)}")
    
    result = subprocess.run(cmd, capture_output=True, text=True)
    
    if result.returncode != 0:
        print(f"Compilation failed:\n{result.stderr}")
        return False
    
    print(f"Success: {output}")
    return True

if __name__ == '__main__':
    if build():
        sys.exit(0)
    else:
        sys.exit(1)
