#!/usr/bin/env python3
"""
WordCard 电子书解析模块
基于 libmobi (MOBI/AZW3) + MuPDF (PDF) 的 C++ Wrapper
通过 ctypes FFI 调用编译好的 .so 共享库
"""
import os
import ctypes
from pathlib import Path

# ========================================================================
# 配置路径
# ========================================================================

BASE_DIR = Path(__file__).parent.resolve()
MOBI_SO = str(BASE_DIR / "libs" / "libmobiparse.so")
PDF_SO = str(BASE_DIR / "libs" / "libpdfparse.so")


# ========================================================================
# MOBI/AZW3 解析器
# ========================================================================

class MobiParser:
    """MOBI/AZW3 电子书解析器"""

    def __init__(self):
        self._lib = None
        self._handle = None

    def _load_library(self):
        """加载 libmobiparse.so"""
        if self._lib is not None:
            return self._lib

        if not os.path.exists(MOBI_SO):
            raise FileNotFoundError(
                f"MOBI 解析库不存在: {MOBI_SO}\n"
                f"请先编译: cd importer/wrappers && make mobi"
            )

        self._lib = ctypes.CDLL(MOBI_SO)

        # mobi_open(const char*) -> void*
        self._lib.mobi_open.argtypes = [ctypes.c_char_p]
        self._lib.mobi_open.restype = ctypes.c_void_p

        # mobi_extract_text(void*, char**, size_t*) -> int
        self._lib.mobi_extract_text.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_char_p),
            ctypes.POINTER(ctypes.c_size_t)
        ]
        self._lib.mobi_extract_text.restype = ctypes.c_int

        # mobi_get_metadata(void*, char*, size_t, char*, size_t) -> int
        self._lib.mobi_get_metadata.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p, ctypes.c_size_t,
            ctypes.c_char_p, ctypes.c_size_t
        ]
        self._lib.mobi_get_metadata.restype = ctypes.c_int

        # mobi_free_text(char*) -> void
        self._lib.mobi_free_text.argtypes = [ctypes.c_char_p]
        self._lib.mobi_free_text.restype = None

        # mobi_close(void*) -> void
        self._lib.mobi_close.argtypes = [ctypes.c_void_p]
        self._lib.mobi_close.restype = None

        return self._lib

    def open(self, path: str):
        """打开 MOBI/AZW3 文件

        Args:
            path: 文件路径

        Returns:
            self（链式调用）
        """
        lib = self._load_library()
        self._handle = lib.mobi_open(path.encode("utf-8"))
        if not self._handle:
            raise RuntimeError(f"无法打开 MOBI 文件: {path}")
        return self

    def extract_text(self) -> str:
        """提取纯文本内容

        Returns:
            文本内容
        """
        if not self._handle:
            raise RuntimeError("文件未打开")

        text_ptr = ctypes.c_char_p()
        text_len = ctypes.c_size_t()

        ret = self._lib.mobi_extract_text(
            self._handle,
            ctypes.byref(text_ptr),
            ctypes.byref(text_len)
        )
        if ret != 0 or not text_ptr.value:
            return ""

        # 复制文本后释放指针（C 层由 close 统一释放内存，这里仅读取）
        text = text_ptr.value.decode("utf-8", errors="ignore")
        return text

    def get_metadata(self) -> dict:
        """获取元数据

        Returns:
            {"title": "...", "author": "..."}
        """
        if not self._handle:
            raise RuntimeError("文件未打开")

        title_buf = ctypes.create_string_buffer(512)
        author_buf = ctypes.create_string_buffer(512)

        self._lib.mobi_get_metadata(
            self._handle,
            title_buf, 512,
            author_buf, 512
        )

        return {
            "title": title_buf.value.decode("utf-8", errors="ignore"),
            "author": author_buf.value.decode("utf-8", errors="ignore"),
        }

    def close(self):
        """关闭文件并释放资源"""
        if self._handle and self._lib:
            self._lib.mobi_close(self._handle)
            self._handle = None

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()
        return False


# ========================================================================
# PDF 解析器
# ========================================================================

class PdfParser:
    """PDF 文档解析器"""

    def __init__(self):
        self._lib = None
        self._handle = None

    def _load_library(self):
        """加载 libpdfparse.so"""
        if self._lib is not None:
            return self._lib

        if not os.path.exists(PDF_SO):
            raise FileNotFoundError(
                f"PDF 解析库不存在: {PDF_SO}\n"
                f"请先编译: cd importer/wrappers && make pdf"
            )

        self._lib = ctypes.CDLL(PDF_SO)

        # pdf_open(const char*) -> void*
        self._lib.pdf_open.argtypes = [ctypes.c_char_p]
        self._lib.pdf_open.restype = ctypes.c_void_p

        # pdf_get_page_count(void*) -> int
        self._lib.pdf_get_page_count.argtypes = [ctypes.c_void_p]
        self._lib.pdf_get_page_count.restype = ctypes.c_int

        # pdf_extract_text(void*, char**, size_t*) -> int
        self._lib.pdf_extract_text.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_char_p),
            ctypes.POINTER(ctypes.c_size_t)
        ]
        self._lib.pdf_extract_text.restype = ctypes.c_int

        # pdf_free_text(char*) -> void
        self._lib.pdf_free_text.argtypes = [ctypes.c_char_p]
        self._lib.pdf_free_text.restype = None

        # pdf_close(void*) -> void
        self._lib.pdf_close.argtypes = [ctypes.c_void_p]
        self._lib.pdf_close.restype = None

        return self._lib

    def open(self, path: str):
        """打开 PDF 文件

        Args:
            path: 文件路径

        Returns:
            self（链式调用）
        """
        lib = self._load_library()
        self._handle = lib.pdf_open(path.encode("utf-8"))
        if not self._handle:
            raise RuntimeError(f"无法打开 PDF 文件: {path}")
        return self

    @property
    def page_count(self) -> int:
        """页数"""
        if not self._handle:
            raise RuntimeError("文件未打开")
        return self._lib.pdf_get_page_count(self._handle)

    def extract_text(self) -> str:
        """提取所有页面的纯文本

        Returns:
            文本内容（页面间用 --- Page Break --- 分隔）
        """
        if not self._handle:
            raise RuntimeError("文件未打开")

        text_ptr = ctypes.c_char_p()
        text_len = ctypes.c_size_t()

        ret = self._lib.pdf_extract_text(
            self._handle,
            ctypes.byref(text_ptr),
            ctypes.byref(text_len)
        )
        if ret != 0 or not text_ptr.value:
            return ""

        text = text_ptr.value.decode("utf-8", errors="ignore")
        return text

    def close(self):
        """关闭文件并释放资源"""
        if self._handle and self._lib:
            self._lib.pdf_close(self._handle)
            self._handle = None

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()
        return False


# ========================================================================
# 便捷函数
# ========================================================================

def parse_mobi(path: str) -> dict:
    """解析 MOBI/AZW3 文件，返回文本和元数据

    Args:
        path: 文件路径

    Returns:
        {"title": "...", "author": "...", "text": "..."}
    """
    with MobiParser() as parser:
        parser.open(path)
        result = parser.get_metadata()
        result["text"] = parser.extract_text()
        return result


def parse_pdf(path: str) -> dict:
    """解析 PDF 文件，返回文本和页数

    Args:
        path: 文件路径

    Returns:
        {"page_count": N, "text": "..."}
    """
    with PdfParser() as parser:
        parser.open(path)
        return {
            "page_count": parser.page_count,
            "text": parser.extract_text(),
        }


def parse_epub(path: str) -> dict:
    """解析 EPUB 文件，返回文本和页数

    底层复用 MuPDF 的文档解析能力，支持 EPUB/FB2 等格式。

    Args:
        path: 文件路径

    Returns:
        {"page_count": N, "text": "..."}
    """
    with PdfParser() as parser:
        parser.open(path)
        return {
            "page_count": parser.page_count,
            "text": parser.extract_text(),
        }


# ========================================================================
# 测试
# ========================================================================

if __name__ == "__main__":
    import sys

    print("=== WordCard E-book Parser Test ===\n")

    # 测试库文件存在
    print("1. Library files")
    print(f"   MOBI SO: {MOBI_SO} -> {'OK' if os.path.exists(MOBI_SO) else 'MISSING'}")
    print(f"   PDF SO:  {PDF_SO} -> {'OK' if os.path.exists(PDF_SO) else 'MISSING'}")

    # 测试加载
    print("\n2. Library loading")
    try:
        mobi = MobiParser()
        mobi._load_library()
        print("   libmobiparse.so: loaded OK")
    except Exception as e:
        print(f"   libmobiparse.so: {e}")

    try:
        pdf = PdfParser()
        pdf._load_library()
        print("   libpdfparse.so: loaded OK")
    except Exception as e:
        print(f"   libpdfparse.so: {e}")

    # 如果传入文件路径，测试解析
    if len(sys.argv) > 1:
        filepath = sys.argv[1]
        ext = os.path.splitext(filepath)[1].lower()

        print(f"\n3. Parsing: {filepath}")
        if ext in (".mobi", ".azw", ".azw3"):
            result = parse_mobi(filepath)
            print(f"   Title: {result['title']}")
            print(f"   Author: {result['author']}")
            print(f"   Text length: {len(result['text'])} chars")
        elif ext == ".pdf":
            result = parse_pdf(filepath)
            print(f"   Pages: {result['page_count']}")
            print(f"   Text length: {len(result['text'])} chars")
            print(f"   Preview: {result['text'][:200]}...")
        elif ext == ".epub":
            result = parse_epub(filepath)
            print(f"   Pages: {result['page_count']}")
            print(f"   Text length: {len(result['text'])} chars")
            print(f"   Preview: {result['text'][:200]}...")
        else:
            print(f"   Unknown format: {ext}")

    print("\n=== Test Complete ===")
