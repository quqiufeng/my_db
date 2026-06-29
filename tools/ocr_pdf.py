#!/usr/bin/env python3
"""
OCR PDF Preprocessor — 使用百度 Unlimited-OCR 对 PDF 进行高精度识别。

输出格式（stdout）：
  第一行: ---OCR_CHAPTERS:<json>---
          {"chapters":[{"title":"...","level":1,"page":0},...]}
  后续:  文本内容，页面间用 --- Page Break --- 分隔
  stderr: 日志信息

用法：
  %s <pdf_path> [--dpi 300]

  --dpi N     转换 PDF 为图片时的 DPI（默认 200，值越高越清晰但越慢）
              对于文字清晰的 PDF，150 就足够；对于扫描件，300 更好。

示例：
  %s ~/document.pdf
  %s ~/scan.pdf --dpi 300
"""

import json
import os
import sys
import tempfile
import time
import shutil

import fitz  # PyMuPDF


def pdf_to_images(pdf_path: str, dpi: int = 200) -> list[str]:
    """将 PDF 每页转换为 PNG 图片，返回图片路径列表。"""
    doc = fitz.open(pdf_path)
    tmp_dir = tempfile.mkdtemp(prefix="ocr_pdf_")
    mat = fitz.Matrix(dpi / 72, dpi / 72)
    paths = []
    for i, page in enumerate(doc):
        out = os.path.join(tmp_dir, f"page_{i+1:04d}.png")
        pix = page.get_pixmap(matrix=mat)
        pix.save(out)
        paths.append(out)
        sys.stderr.write(f"\r  [OCR] 转换页面 {i+1}/{len(doc)}")
        sys.stderr.flush()
    doc.close()
    sys.stderr.write("\n")
    return paths


def extract_chapters_from_pdf(pdf_path: str) -> list[dict]:
    """从 PDF 文件本身提取目录/书签信息（比 OCR 输出更准确）。"""
    try:
        doc = fitz.open(pdf_path)
        toc = doc.get_toc(simple=False)
        doc.close()
        chapters = []
        if toc:
            for item in toc:
                level, title, page = item[0], item[1], item[2]
                chapters.append({
                    "title": title,
                    "level": level - 1,  # PyMuPDF level 从 1 开始
                    "page": page - 1      # PyMuPDF page 从 1 开始
                })
        return chapters
    except Exception as e:
        sys.stderr.write(f"  [OCR] 提取目录失败: {e}\n")
        return []


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print(__doc__ % (sys.argv[0], sys.argv[0], sys.argv[0]))
        sys.exit(1)

    pdf_path = sys.argv[1]

    dpi = 200
    for i, arg in enumerate(sys.argv):
        if arg == "--dpi" and i + 1 < len(sys.argv):
            try:
                dpi = int(sys.argv[i + 1])
            except ValueError:
                pass

    if not os.path.isfile(pdf_path):
        sys.stderr.write(f"[ERROR] 文件不存在: {pdf_path}\n")
        sys.exit(1)

    sys.stderr.write(f"[OCR] 开始处理: {pdf_path}\n")
    sys.stderr.write(f"[OCR] DPI: {dpi}\n")

    # === 1. PDF → 图片 ===
    sys.stderr.write("[OCR] 转换 PDF 为图片...\n")
    t0 = time.time()
    image_paths = pdf_to_images(pdf_path, dpi=dpi)
    t1 = time.time()
    sys.stderr.write(f"[OCR] PDF 转图片完成: {len(image_paths)} 页, 耗时 {t1-t0:.1f}s\n")

    # === 2. 提取章节信息（从 PDF 目录） ===
    chapters = extract_chapters_from_pdf(pdf_path)
    sys.stderr.write(f"[OCR] 提取到 {len(chapters)} 个章节\n")

    # === 3. 加载 Unlimited-OCR 模型 ===
    sys.stderr.write("[OCR] 加载 Unlimited-OCR 模型...\n")
    t0 = time.time()

    model_path = "/data/models/baidu/Unlimited-OCR"
    if not os.path.isdir(model_path):
        model_path = "baidu/Unlimited-OCR"

    from transformers import AutoModel, AutoTokenizer
    import torch

    tokenizer = AutoTokenizer.from_pretrained(
        model_path,
        trust_remote_code=True
    )
    model = AutoModel.from_pretrained(
        model_path,
        trust_remote_code=True,
        use_safetensors=True,
        torch_dtype=torch.bfloat16,
    ).eval().cuda()

    t1 = time.time()
    sys.stderr.write(f"[OCR] 模型加载完成, 耗时 {t1-t0:.1f}s\n")

    # === 4. OCR 推理 (base 模式：多页批量处理) ===
    sys.stderr.write("[OCR] OCR 推理中...\n")
    t0 = time.time()

    # infer_multi 需要有效的 output_path（即使 save_results=False）
    tmp_output = tempfile.mkdtemp(prefix="ocr_result_")

    result_text, _ = model.infer_multi(
        tokenizer,
        prompt="<image>Multi page parsing.",
        image_files=image_paths,
        output_path=tmp_output,
        image_size=1024,
        max_length=32768,
        no_repeat_ngram_size=35,
        ngram_window=1024,
        save_results=False,
    )

    t1 = time.time()
    sys.stderr.write(f"\n[OCR] 推理完成, 耗时 {t1-t0:.1f}s\n")

    # 清理暂存目录
    shutil.rmtree(tmp_output, ignore_errors=True)

    # === 5. 处理输出文本 ===
    # 模型输出使用 <PAGE> 分隔不同页面
    all_text = result_text

    # 将 <PAGE> 标记转换为 --- Page Break ---（与现有 import_book 格式兼容）
    if "<PAGE>" in all_text:
        pages = all_text.split("<PAGE>")
        converted = []
        for p in pages:
            p = p.strip()
            if p:
                converted.append(p)
        all_text = "\n\n--- Page Break ---\n\n".join(converted)
        # 末尾不加分隔符
        if all_text.endswith("--- Page Break ---\n\n"):
            all_text = all_text[:-len("--- Page Break ---\n\n")]

    # === 5.5 清洗文本：去除 <|det|> 检测标签，保留纯内容 ===
    import re
    # 去除 <|det|>type [bbox]<|/det|> 标签，只保留实际文本内容
    all_text = re.sub(r'<\|det\|>[^<]+<\|/det\|>', '', all_text)
    # 去除 <PAGE> 标记（已在上一步转换）
    all_text = all_text.replace('<PAGE>', '')
    # 清理多余空行
    all_text = re.sub(r'\n{3,}', '\n\n', all_text)
    all_text = all_text.strip()

    # 如果 PDF 没有目录，从 OCR 文本中尝试提取章节
    if not chapters and all_text.strip():
        # 简单提取：查找 Markdown H1/H2 标题
        extracted = []
        lines = all_text.split("\n")
        page_counter = 0
        for line in lines:
            if line.strip() == "--- Page Break ---":
                page_counter += 1
            elif line.strip().startswith("# ") and not line.strip().startswith("## "):
                title = line.strip()[2:].strip()
                if title:
                    extracted.append({"title": title, "level": 0, "page": page_counter})
            elif line.strip().startswith("## "):
                title = line.strip()[3:].strip()
                if title:
                    extracted.append({"title": title, "level": 1, "page": page_counter})
        if extracted:
            chapters = extracted
            sys.stderr.write(f"[OCR] 从文本提取到 {len(chapters)} 个章节\n")

    # === 6. 输出到 stdout ===
    # 第一行：章节信息（JSON）
    chapters_json = json.dumps({"chapters": chapters}, ensure_ascii=False)
    sys.stdout.write(f"---OCR_CHAPTERS:{chapters_json}---\n")
    # 后续：文本内容
    sys.stdout.write(all_text)
    sys.stdout.flush()

    # === 7. 清理临时图片文件 ===
    tmp_dir = os.path.dirname(image_paths[0]) if image_paths else ""
    if tmp_dir and os.path.isdir(tmp_dir):
        shutil.rmtree(tmp_dir, ignore_errors=True)

    sys.stderr.write("[OCR] 完成\n")


if __name__ == "__main__":
    main()
