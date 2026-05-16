# E-book Parser Wrappers

> 独立维护的 C++ Wrapper，用于将 libmobi (MOBI/AZW3) 和 MuPDF (PDF) 封装为 C 接口供 Python ctypes 调用。

---

## 文件说明

| 文件 | 说明 |
|------|------|
| `mobi_wrapper.cpp` | libmobi 封装：打开/提取文本/获取元数据/关闭 |
| `pdf_wrapper.cpp` | MuPDF 封装：打开/提取文本/获取页数/关闭 |
| `Makefile` | 编译脚本 |

## 依赖安装

```bash
# 系统依赖
sudo apt-get install -y \
  autoconf automake libtool pkg-config \
  libxml2-dev zlib1g-dev \
  liblcms2-dev libjpeg-dev libopenjp2-7-dev \
  libjbig2dec0-dev libpng-dev libfreetype6-dev \
  libharfbuzz-dev libmujs-dev
```

## 源码位置

- libmobi: `/opt/libmobi`
- MuPDF: `/opt/mupdf`

## 编译步骤

```bash
cd /opt/WordCard/importer/wrappers
make clean && make -j$(nproc)
```

输出：
- `../libs/libmobiparse.so`  (MOBI/AZW3 解析)
- `../libs/libpdfparse.so`   (PDF 解析)

## Python 调用方式

```python
from importer import parse_mobi, parse_pdf

# 解析 MOBI
result = parse_mobi("book.mobi")
print(result["title"], result["author"])
print(result["text"][:500])

# 解析 PDF
result = parse_pdf("document.pdf")
print(result["page_count"])
print(result["text"][:500])
```

或使用上下文管理器：

```python
from importer import MobiParser, PdfParser

with MobiParser() as parser:
    parser.open("book.azw3")
    meta = parser.get_metadata()
    text = parser.extract_text()

with PdfParser() as parser:
    parser.open("doc.pdf")
    print(parser.page_count)
    text = parser.extract_text()
```

## 修改记录

| 日期 | 修改内容 |
|------|----------|
| 2026-05-15 | 初始创建，集成 libmobi + MuPDF |

---

**注意**：
- `.so` 文件依赖编译时的静态库，但已静态链接，运行时无需额外依赖
- 如果修改了 `.cpp` 文件，需要重新 `make`
- 文件路径在 Python 层传入，不在 C 层硬编码
