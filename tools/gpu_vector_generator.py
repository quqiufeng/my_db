#!/usr/bin/env python3
"""
GPU 向量生成器包装脚本
确保使用正确的 CUDA 库路径
"""
import os
import sys

# 必须在导入其他模块前设置
conda_env = "/home/dministrator/anaconda3/envs/dl"
cuda_lib = f"{conda_env}/lib"
onnx_lib = f"{conda_env}/lib/python3.10/site-packages/onnxruntime/capi"

ld_path = os.environ.get("LD_LIBRARY_PATH", "")
os.environ["LD_LIBRARY_PATH"] = f"{cuda_lib}:{onnx_lib}:{ld_path}"

# 现在导入其他模块
sys.path.insert(0, '/home/dministrator/my_db')

from tools.batch_vector_generator import main

if __name__ == '__main__':
    main()
