#!/usr/bin/env python3
"""
GitHub 源码导入工具 —— 复用 code_bin 的 ctags 解析思路，存入 my_db KV Cache

思路（同 code_bin ctags_parser.c）：
1. git clone --depth 1 {repo_url} /tmp/repo
2. cd /tmp/repo && ctags --output-format=json --fields=+nK --extras=+r --sort=no -R .
3. 解析 JSON 行，提取：name, path, kind, line, pattern/signature
4. 将符号索引 + 源码内容 写入 KV Cache

存储格式：
  /github/{owner}/{repo}/_meta/*     — 元数据
  /github/{owner}/{repo}/symbols/*   — 符号索引（由 ctags 提取）
  /github/{owner}/{repo}/src/*       — 源码文件内容

用法：
  python3 tools/import_github.py https://github.com/redis/redis /github/redis
  python3 tools/import_github.py https://github.com/python/cpython /github/cpython --max-file-size 50000
"""

import os
import sys
import subprocess
import tempfile
import shutil
import json
import re
from pathlib import Path
from urllib.parse import urlparse

# 加载 mydb cache 模块
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from mydb.cache import open_cache

# ========================================================================
# 配置
# ========================================================================

# 默认忽略的文件/目录（同 code_bin）
DEFAULT_EXCLUDES = [
    '.git', 'node_modules', 'vendor', 'build', 'dist', 'target',
    '__pycache__', '.pytest_cache', '*.egg-info', '.tox',
    'third_party', '3rdparty', 'third-party',
    'CMakeFiles', '*.cmake', 'Makefile', 'configure',
    '*.min.js', '*.min.css', '*.map',
]

# 最大文件大小（字节），超过则跳过内容存储，只存索引
DEFAULT_MAX_FILE_SIZE = 100 * 1024  # 100KB

# 支持的源码扩展名
SOURCE_EXTENSIONS = {
    '.c', '.h', '.cpp', '.cc', '.cxx', '.hpp',
    '.py', '.pyx', '.pxd',
    '.rs', '.go', '.java', '.kt',
    '.js', '.ts', '.jsx', '.tsx',
    '.rb', '.php', '.swift',
    '.cs', '.fs', '.fsx',
    '.scala', '.clj',
    '.lua', '.vim',
    '.sh', '.bash', '.zsh',
    '.pl', '.pm',
    '.r', '.R',
    '.m', '.mm',  # Objective-C
}

# ========================================================================
# 工具函数
# ========================================================================

def find_ctags():
    """查找可用的 ctags 命令（优先 universal-ctags）"""
    for cmd in ['ctags-universal', 'uctags', 'ctags']:
        try:
            result = subprocess.run([cmd, '--version'], capture_output=True, text=True, timeout=5)
            if result.returncode == 0 and 'Universal' in result.stdout:
                return cmd
        except (FileNotFoundError, subprocess.TimeoutExpired):
            continue
    return 'ctags'  # fallback


def run_ctags(repo_path):
    """运行 ctags，返回 JSON 行列表（同 code_bin ctags_parser_parse）"""
    ctags_cmd = find_ctags()
    cmd = [
        ctags_cmd,
        '--output-format=json',
        '--fields=+nK',      # line number + kind
        '--extras=+r',       # reference tags
        '--sort=no',
        '-R',
        '.',
    ]
    
    # 添加排除模式
    for pat in DEFAULT_EXCLUDES:
        cmd.extend(['--exclude=' + pat])
    
    try:
        result = subprocess.run(
            cmd,
            cwd=repo_path,
            capture_output=True,
            text=True,
            timeout=300,
        )
        if result.returncode != 0:
            print(f"  ctags warning (exit {result.returncode}): {result.stderr[:200]}", file=sys.stderr)
        
        lines = result.stdout.strip().split('\n')
        return [l for l in lines if l.strip()]
    except FileNotFoundError:
        print("错误：未找到 universal-ctags，请先安装", file=sys.stderr)
        print("  Ubuntu/Debian: sudo apt-get install universal-ctags", file=sys.stderr)
        print("  macOS: brew install universal-ctags", file=sys.stderr)
        return []
    except subprocess.TimeoutExpired:
        print("错误：ctags 解析超时", file=sys.stderr)
        return []


def parse_ctags_line(line):
    """
    解析单行 ctags JSON 输出（同 code_bin parse_ctags_line）
    
    输入: {"_type":"tag","name":"main","path":"src/main.c","line":10,"kind":"function","pattern":"/^int main(int argc..."}
    返回: dict 或 None
    """
    try:
        obj = json.loads(line)
    except json.JSONDecodeError:
        return None
    
    if obj.get('_type') != 'tag':
        return None
    
    return {
        'name': obj.get('name', ''),
        'path': obj.get('path', ''),
        'kind': obj.get('kind', 'unknown'),
        'line': obj.get('line', 0),
        'pattern': obj.get('pattern', ''),
        'signature': obj.get('signature', ''),
        'scope': obj.get('scope', ''),
    }


def extract_kind_category(kind):
    """将 ctags kind 分类（同 code_bin convert_kind）"""
    kind_map = {
        'function': 'function',
        'method': 'function',
        'class': 'class',
        'struct': 'struct',
        'union': 'struct',
        'enum': 'enum',
        'enumerator': 'enum',
        'typedef': 'typedef',
        'variable': 'variable',
        'member': 'variable',
        'field': 'variable',
        'macro': 'macro',
        'namespace': 'namespace',
        'interface': 'interface',
        'package': 'namespace',
        'module': 'namespace',
        'local': 'variable',
        'parameter': 'variable',
    }
    return kind_map.get(kind.lower(), 'unknown')


def is_source_file(filepath):
    """检查是否为源码文件"""
    ext = Path(filepath).suffix.lower()
    return ext in SOURCE_EXTENSIONS


def get_language_by_ext(filepath):
    """根据扩展名推断语言"""
    ext = Path(filepath).suffix.lower()
    lang_map = {
        '.c': 'c', '.h': 'c',
        '.cpp': 'cpp', '.cc': 'cpp', '.cxx': 'cpp', '.hpp': 'cpp',
        '.py': 'python', '.pyx': 'python', '.pxd': 'python',
        '.rs': 'rust',
        '.go': 'go',
        '.java': 'java',
        '.kt': 'kotlin',
        '.js': 'javascript', '.ts': 'typescript',
        '.jsx': 'javascript', '.tsx': 'typescript',
        '.rb': 'ruby',
        '.php': 'php',
        '.swift': 'swift',
        '.cs': 'csharp',
        '.scala': 'scala',
        '.lua': 'lua',
        '.sh': 'bash', '.bash': 'bash',
        '.pl': 'perl', '.pm': 'perl',
    }
    return lang_map.get(ext, 'unknown')


def clone_repo(repo_url, tmpdir):
    """浅克隆仓库"""
    cmd = ['git', 'clone', '--depth', '1', repo_url, tmpdir]
    print(f"Cloning {repo_url} ...")
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"克隆失败: {result.stderr}", file=sys.stderr)
        return False
    return True


def get_repo_metadata(repo_path):
    """获取仓库元数据"""
    meta = {}
    
    # git remote url
    try:
        result = subprocess.run(
            ['git', 'remote', 'get-url', 'origin'],
            cwd=repo_path, capture_output=True, text=True
        )
        if result.returncode == 0:
            meta['url'] = result.stdout.strip()
    except Exception:
        pass
    
    # git commit
    try:
        result = subprocess.run(
            ['git', 'rev-parse', 'HEAD'],
            cwd=repo_path, capture_output=True, text=True
        )
        if result.returncode == 0:
            meta['commit'] = result.stdout.strip()[:12]
    except Exception:
        pass
    
    # 统计文件数和语言
    lang_counts = {}
    total_files = 0
    for root, dirs, files in os.walk(repo_path):
        # 跳过排除目录
        dirs[:] = [d for d in dirs if d not in DEFAULT_EXCLUDES and not d.startswith('.')]
        
        for f in files:
            if is_source_file(f):
                total_files += 1
                lang = get_language_by_ext(f)
                lang_counts[lang] = lang_counts.get(lang, 0) + 1
    
    meta['total_files'] = total_files
    meta['languages'] = lang_counts
    
    return meta


def parse_repo_url(repo_url):
    """从 URL 解析 owner 和 repo"""
    parsed = urlparse(repo_url)
    path = parsed.path.strip('/')
    parts = path.split('/')
    if len(parts) >= 2:
        return parts[0], parts[1]
    return 'unknown', 'unknown'


# ========================================================================
# 导入主逻辑
# ========================================================================

def import_github(repo_url, cache_dir, namespace=None, max_file_size=DEFAULT_MAX_FILE_SIZE):
    """
    导入 GitHub 仓库到 KV Cache
    
    Args:
        repo_url: GitHub 仓库 URL
        cache_dir: KV Cache 目录
        namespace: 存储路径（默认 /github/{owner}/{repo}）
        max_file_size: 最大文件大小（超过只存索引）
    """
    
    owner, repo = parse_repo_url(repo_url)
    if not namespace:
        namespace = f"/github/{owner}/{repo}"
    
    print(f"\n{'='*60}")
    print(f"GitHub Import: {owner}/{repo}")
    print(f"Cache: {cache_dir}")
    print(f"Namespace: {namespace}")
    print(f"{'='*60}\n")
    
    # 1. 克隆仓库
    tmpdir = tempfile.mkdtemp(prefix='github_import_')
    try:
        if not clone_repo(repo_url, tmpdir):
            return False
        
        # 2. 获取元数据
        print("Scanning repository...")
        meta = get_repo_metadata(tmpdir)
        print(f"  Files: {meta['total_files']}")
        print(f"  Languages: {meta['languages']}")
        
        # 3. 运行 ctags
        print("\nRunning ctags (this may take a while)...")
        ctags_lines = run_ctags(tmpdir)
        print(f"  ctags output: {len(ctags_lines)} lines")
        
        # 4. 打开 KV Cache
        cache = open_cache(cache_dir, 500 * 1024 * 1024)  # 500MB
        print(f"\nCache opened: {cache}\n")
        
        # 5. 存储元数据
        cache.set(f"{namespace}/_meta/url", meta.get('url', repo_url), 0)
        if meta.get('commit'):
            cache.set(f"{namespace}/_meta/commit", meta['commit'], 0)
        cache.set_json(f"{namespace}/_meta/languages", meta.get('languages', {}), 0)
        cache.set(f"{namespace}/_meta/total_files", str(meta.get('total_files', 0)), 0)
        
        # 6. 解析并存储符号
        print("Importing symbols...")
        symbols_by_file = {}  # path -> [symbols]
        symbol_count = 0
        skipped_count = 0
        
        for line in ctags_lines:
            sym = parse_ctags_line(line)
            if not sym:
                continue
            
            # 跳过无效符号
            if not sym['name'] or not sym['path']:
                skipped_count += 1
                continue
            
            # 存储符号索引
            kind_cat = extract_kind_category(sym['kind'])
            sym_key = f"{namespace}/symbols/{sym['name']}"
            
            # 同一名字可能有多个（重载），用 JSON 数组
            existing = cache.get_json(sym_key)
            if existing is None:
                existing = []
            
            sym_entry = {
                't': kind_cat,
                'file': sym['path'],
                'line': sym['line'],
                'kind': sym['kind'],
            }
            if sym['signature']:
                sym_entry['signature'] = sym['signature']
            if sym['scope']:
                sym_entry['scope'] = sym['scope']
            
            existing.append(sym_entry)
            cache.set_json(sym_key, existing, 0)
            symbol_count += 1
            
            # 按文件分组（用于后续源码关联）
            filepath = sym['path']
            if filepath not in symbols_by_file:
                symbols_by_file[filepath] = []
            symbols_by_file[filepath].append(sym_entry)
        
        print(f"  Imported: {symbol_count} symbols")
        print(f"  Skipped: {skipped_count}")
        
        # 7. 存储源码文件内容
        print("\nImporting source files...")
        file_count = 0
        skipped_files = 0
        
        for root, dirs, files in os.walk(tmpdir):
            dirs[:] = [d for d in dirs if d not in DEFAULT_EXCLUDES and not d.startswith('.')]
            
            for filename in files:
                if not is_source_file(filename):
                    continue
                
                full_path = os.path.join(root, filename)
                rel_path = os.path.relpath(full_path, tmpdir)
                
                try:
                    size = os.path.getsize(full_path)
                    if size > max_file_size:
                        # 只存元数据，不存内容
                        cache.set_json(
                            f"{namespace}/src/{rel_path}",
                            {
                                't': 'code',
                                'lang': get_language_by_ext(filename),
                                'size': size,
                                'truncated': True,
                            },
                            0
                        )
                        skipped_files += 1
                        continue
                    
                    with open(full_path, 'r', encoding='utf-8', errors='ignore') as f:
                        content = f.read()
                    
                    # 关联该文件的符号
                    file_syms = symbols_by_file.get(rel_path, [])
                    
                    cache.set_json(
                        f"{namespace}/src/{rel_path}",
                        {
                            't': 'code',
                            'lang': get_language_by_ext(filename),
                            'size': size,
                            'lines': content.count('\n') + 1,
                            'content': content,
                            'symbols': file_syms[:50],  # 最多存 50 个符号
                        },
                        0
                    )
                    file_count += 1
                    
                except Exception as e:
                    print(f"  Warning: failed to read {rel_path}: {e}", file=sys.stderr)
                    skipped_files += 1
        
        print(f"  Imported: {file_count} files")
        print(f"  Skipped (too large): {skipped_files}")
        
        # 8. 存储文件树结构
        structure = []
        for root, dirs, files in os.walk(tmpdir):
            dirs[:] = [d for d in dirs if d not in DEFAULT_EXCLUDES and not d.startswith('.')]
            rel_root = os.path.relpath(root, tmpdir)
            if rel_root == '.':
                rel_root = ''
            
            for f in files:
                if is_source_file(f):
                    structure.append(os.path.join(rel_root, f) if rel_root else f)
        
        cache.set_json(f"{namespace}/_meta/structure", structure, 0)
        
        # 9. 刷盘并统计
        cache.sync()
        print(f"\n{'='*60}")
        print(f"Import complete!")
        print(f"  Total entries: {cache.count}")
        print(f"  Memory used: {cache.memory_used / 1024 / 1024:.1f} MB")
        print(f"{'='*60}\n")
        
        cache.close()
        return True
        
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


# ========================================================================
# CLI
# ========================================================================

def main():
    import argparse
    
    parser = argparse.ArgumentParser(
        description='Import GitHub repository into my_db KV Cache',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  %(prog)s https://github.com/redis/redis /opt/code_caches/redis_cache
  %(prog)s https://github.com/python/cpython /opt/code_caches/cpython_cache --namespace /github/cpython
  %(prog)s https://github.com/torvalds/linux /opt/code_caches/linux_cache --max-file-size 200000
        """
    )
    
    parser.add_argument('repo_url', help='GitHub 仓库 URL')
    parser.add_argument('cache_dir', help='KV Cache 目录')
    parser.add_argument('--namespace', '-n', help='存储路径（默认 /github/{owner}/{repo}）')
    parser.add_argument('--max-file-size', '-m', type=int, default=DEFAULT_MAX_FILE_SIZE,
                        help=f'最大文件大小（字节，默认 {DEFAULT_MAX_FILE_SIZE}）')
    
    args = parser.parse_args()
    
    success = import_github(
        args.repo_url,
        args.cache_dir,
        namespace=args.namespace,
        max_file_size=args.max_file_size,
    )
    
    sys.exit(0 if success else 1)


if __name__ == '__main__':
    main()
