#!/usr/bin/env python3
"""
ONNX Embedder Python FFI Wrapper
复用 C 端的 onnx_embedder.c，不依赖 sentence-transformers

用法:
    from mydb.onnx_embedder import OnnxEmbedder
    
    embedder = OnnxEmbedder("models/all-MiniLM-L6-v2/model.onnx", 
                            "models/all-MiniLM-L6-v2/vocab.txt")
    vector = embedder.encode("def hello(): pass")
    # vector: list[float] of length 384
"""

import ctypes
import os
import sys

# Preload TensorRT dependencies with RTLD_GLOBAL so ONNX Runtime provider can find symbols
# This avoids LD_LIBRARY_PATH requirement
import ctypes as _ctypes

# Set dlopen flags to RTLD_GLOBAL before loading shared libraries
# This ensures symbols are visible to subsequently loaded libraries (like ONNX providers)
_RTLD_GLOBAL = 0x00100
_RTLD_NOW = 0x00002
_prev_dlopen_flags = sys.getdlopenflags()
sys.setdlopenflags(_RTLD_NOW | _RTLD_GLOBAL)

try:
    # Load in dependency order: cuDNN first, then TensorRT core
    _ctypes.CDLL("/home/dministrator/my_db/libcudnn.so.9")
    _ctypes.CDLL("/home/dministrator/my_db/libnvinfer.so.10")
    _ctypes.CDLL("/home/dministrator/my_db/libnvonnxparser.so.10")
except Exception:
    pass
finally:
    # Restore previous flags
    sys.setdlopenflags(_prev_dlopen_flags)

# 加载动态库
# 优先查找 libonnx_embedder.so（我们会编译它）
lib_paths = [
    "./libonnx_embedder.so",
    "libonnx_embedder.so",
    "/usr/local/lib/libonnx_embedder.so",
    "/usr/lib/libonnx_embedder.so",
]

_lib = None
for path in lib_paths:
    if os.path.exists(path):
        try:
            _lib = ctypes.CDLL(path)
            break
        except OSError:
            continue

if _lib is None:
    raise RuntimeError(
        f"Cannot load libonnx_embedder.so, tried: {lib_paths}\n"
        "Please build it first: make libonnx_embedder.so"
    )

# ========================================================================
# 配置函数签名
# ========================================================================

_lib.onnx_embedder_init.argtypes = [
    ctypes.c_char_p,  # model_path
    ctypes.c_char_p,  # vocab_path
    ctypes.c_int,     # max_seq_length
    ctypes.c_int,     # dim
]
_lib.onnx_embedder_init.restype = ctypes.c_void_p

_lib.onnx_embedder_encode.argtypes = [
    ctypes.c_void_p,  # embedder
    ctypes.c_char_p,  # text
    ctypes.POINTER(ctypes.c_float),  # vector output
]
_lib.onnx_embedder_encode.restype = ctypes.c_int

_lib.onnx_embedder_free.argtypes = [ctypes.c_void_p]

_lib.onnx_embedder_error.restype = ctypes.c_char_p


class OnnxEmbedder:
    """
    ONNX 嵌入模型封装（C 后端）
    
    复用 src/embedding/onnx_embedder.c，不依赖 Python 的 transformers
    """
    
    def __init__(self, model_path: str = None, vocab_path: str = None, 
                 max_seq_length: int = 128, dim: int = 384):
        """
        初始化 ONNX embedder
        
        Args:
            model_path: ONNX 模型路径（默认 models/all-MiniLM-L6-v2/model.onnx）
            vocab_path: 词汇表路径（默认 models/all-MiniLM-L6-v2/vocab.txt）
            max_seq_length: 最大序列长度
            dim: 输出向量维度
        """
        # 支持环境变量覆盖
        if model_path is None:
            model_path = os.environ.get("EMBEDDING_MODEL", "models/all-MiniLM-L6-v2/model.onnx")
        if vocab_path is None:
            vocab_path = os.environ.get("EMBEDDING_VOCAB", "models/all-MiniLM-L6-v2/vocab.txt")
        
        self.dim = dim
        self._ptr = _lib.onnx_embedder_init(
            model_path.encode('utf-8'),
            vocab_path.encode('utf-8'),
            max_seq_length,
            dim
        )
        
        if not self._ptr:
            error = _lib.onnx_embedder_error()
            raise RuntimeError(f"Failed to init ONNX embedder: {error.decode() if error else 'unknown'}")
    
    def encode(self, text: str) -> list:
        """
        生成文本的嵌入向量
        
        Args:
            text: 输入文本
        
        Returns:
            list[float]: 384 维向量（已 L2 归一化）
        """
        # 分配输出数组
        vector = (ctypes.c_float * self.dim)()
        
        ret = _lib.onnx_embedder_encode(self._ptr, text.encode('utf-8'), vector)
        if ret != 0:
            error = _lib.onnx_embedder_error()
            raise RuntimeError(f"ONNX encode failed: {error.decode() if error else 'unknown'}")
        
        return list(vector)
    
    def encode_batch(self, texts: list, batch_size: int = 32) -> list:
        """
        批量编码
        
        Args:
            texts: 文本列表
            batch_size: 批大小（C 端是单线程，这里只是分块）
        
        Returns:
            list[list[float]]: 向量列表
        """
        results = []
        for text in texts:
            results.append(self.encode(text))
        return results
    
    def close(self):
        """释放资源"""
        if self._ptr:
            _lib.onnx_embedder_free(self._ptr)
            self._ptr = None
    
    def __enter__(self):
        return self
    
    def __exit__(self, *args):
        self.close()
        return False


# 便捷函数
def create_embedder(model_path: str = None, vocab_path: str = None):
    """创建 embedder 的便捷函数"""
    return OnnxEmbedder(model_path, vocab_path)


if __name__ == "__main__":
    # 简单测试
    try:
        embedder = OnnxEmbedder()
        
        texts = [
            "int main() { return 0; }",
            "def hello_world(): pass",
            "class Cache { void get(); };",
        ]
        
        print("Testing ONNX Embedder (C backend):\n")
        for text in texts:
            vector = embedder.encode(text)
            print(f"Text: {text[:40]}...")
            print(f"Vector dim: {len(vector)}")
            print(f"Vector[:5]: {[round(x, 4) for x in vector[:5]]}")
            print()
        
        embedder.close()
        print("Test passed!")
        
    except Exception as e:
        print(f"Test failed: {e}")
        sys.exit(1)
