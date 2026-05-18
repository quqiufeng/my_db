#!/usr/bin/env python3
"""
AI Agent 代码记忆系统 - Namespace 精准定位版

设计哲学：
1. 90% 的场景通过 namespace 层级直接定位，不需要搜索
2. 模糊搜索仅作为兜底，且限定在 namespace 范围内
3. AI 可以通过"仓库→文件→符号"三级路径精确获取代码

存储结构（namespace 层级）：
  /code/{owner}/{repo}/_meta/                    仓库元数据
  /code/{owner}/{repo}/files/{filepath}          文件级索引
  /code/{owner}/{repo}/chunks/{filepath}/{func}  代码片段（精准定位）
  /code/{owner}/{repo}/symbols/{name}            符号表（快速跳转）
  /code/{owner}/{repo}/refs/{name}               引用关系（调用图）
  /code/{owner}/{repo}/vectors/{chunk_id}        语义向量（自然语言兜底）

查询策略（按优先级）：
  1. 精确路径: /code/redis/chunks/src/server.c/main → 直接命中
  2. 符号跳转: /code/redis/symbols/main → 获取定义位置
  3. 文件限定: /code/redis/chunks/src/server.c/* → 浏览文件内所有符号
  4. Namespace 搜索: 在 /code/redis/* 范围内关键词搜索
  5. 全局模糊: 最后的兜底手段
"""

import os
import sys
import json
import hashlib
import subprocess
import tempfile
import shutil
import re
from pathlib import Path
from typing import List, Dict, Optional, Tuple
from dataclasses import dataclass
from urllib.parse import urlparse

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from mydb.cache import open_cache

# ========================================================================
# 配置
# ========================================================================

DEFAULT_EXCLUDES = [
    '.git', 'node_modules', 'vendor', 'build', 'dist', 'target',
    '__pycache__', '.pytest_cache', '*.egg-info', '.tox',
    'third_party', '3rdparty', 'third-party',
    'CMakeFiles', '*.cmake', 'Makefile', 'configure',
    '*.min.js', '*.min.css', '*.map', '*.a', '*.o', '*.so',
]

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
    '.m', '.mm',
}

# ========================================================================
# 代码解析引擎（复用之前的）
# ========================================================================

class CodeParser:
    def __init__(self, repo_path: str):
        self.repo_path = Path(repo_path)
        self.ctags_cmd = self._find_ctags()
        
    def _find_ctags(self):
        for cmd in ['ctags-universal', 'uctags', 'ctags']:
            try:
                result = subprocess.run([cmd, '--version'], capture_output=True, text=True, timeout=5)
                if result.returncode == 0 and 'Universal' in result.stdout:
                    return cmd
            except (FileNotFoundError, subprocess.TimeoutExpired):
                continue
        return 'ctags'
    
    def run_ctags(self):
        cmd = [
            self.ctags_cmd,
            '--output-format=json',
            '--fields=+nKzS',
            '--extras=+r+f',
            '--sort=no',
            '-R', '.',
        ]
        for pat in DEFAULT_EXCLUDES:
            cmd.extend(['--exclude=' + pat])
        
        result = subprocess.run(cmd, cwd=self.repo_path, capture_output=True, text=True, timeout=300)
        lines = result.stdout.strip().split('\n')
        return [json.loads(l) for l in lines if l.strip()]
    
    def extract_scope_range(self, tags: List[dict]) -> Dict[str, Tuple[int, int]]:
        file_tags = {}
        for tag in tags:
            if tag.get('_type') != 'tag':
                continue
            filepath = tag.get('path', '')
            if filepath not in file_tags:
                file_tags[filepath] = []
            file_tags[filepath].append(tag)
        
        ranges = {}
        for filepath, tag_list in file_tags.items():
            tag_list.sort(key=lambda x: x.get('line', 0))
            for i, tag in enumerate(tag_list):
                start_line = tag.get('line', 1)
                end_line = start_line + 50
                current_level = tag.get('level', 0)
                
                for j in range(i + 1, len(tag_list)):
                    next_tag = tag_list[j]
                    next_level = next_tag.get('level', 0)
                    if next_level <= current_level:
                        end_line = next_tag.get('line', start_line + 50) - 1
                        break
                
                key = f"{filepath}:{tag.get('name')}"
                ranges[key] = (start_line, end_line)
        
        return ranges
    
    def parse_file_content(self, filepath: str, start_line: int, end_line: int) -> str:
        full_path = self.repo_path / filepath
        try:
            with open(full_path, 'r', encoding='utf-8', errors='ignore') as f:
                lines = f.readlines()
                start = max(0, start_line - 1)
                end = min(len(lines), end_line)
                return ''.join(lines[start:end])
        except Exception:
            return ""
    
    def extract_docstring(self, content: str, language: str) -> str:
        lines = content.split('\n')
        docs = []
        
        if language in ('python', 'ruby'):
            in_doc = False
            for line in lines[:10]:
                if '"""' in line or "'''" in line:
                    if in_doc:
                        break
                    in_doc = True
                if in_doc:
                    docs.append(line.strip())
        elif language in ('c', 'cpp', 'java', 'go', 'rust', 'javascript'):
            for line in lines[:10]:
                stripped = line.strip()
                if stripped.startswith('//') or stripped.startswith('/*') or stripped.startswith('*'):
                    docs.append(stripped)
                elif stripped and not stripped.startswith('#'):
                    break
        
        return '\n'.join(docs[:5])
    
    def get_language(self, filepath: str) -> str:
        ext = Path(filepath).suffix.lower()
        lang_map = {
            '.c': 'c', '.h': 'c',
            '.cpp': 'cpp', '.cc': 'cpp', '.cxx': 'cpp', '.hpp': 'cpp',
            '.py': 'python', '.pyx': 'python', '.pxd': 'python',
            '.rs': 'rust', '.go': 'go', '.java': 'java', '.kt': 'kotlin',
            '.js': 'javascript', '.ts': 'typescript',
            '.jsx': 'javascript', '.tsx': 'typescript',
            '.rb': 'ruby', '.php': 'php', '.swift': 'swift',
            '.cs': 'csharp', '.scala': 'scala', '.lua': 'lua',
            '.sh': 'bash', '.bash': 'bash', '.pl': 'perl',
        }
        return lang_map.get(ext, 'unknown')
    
    def extract_calls(self, content: str, language: str) -> List[str]:
        calls = set()
        if language in ('c', 'cpp', 'java', 'go', 'rust', 'javascript'):
            pattern = r'\b([a-zA-Z_][a-zA-Z0-9_]*)\s*\('
            for match in re.finditer(pattern, content):
                name = match.group(1)
                if name not in ('if', 'while', 'for', 'switch', 'return', 'sizeof', 'typeof'):
                    calls.add(name)
        elif language == 'python':
            pattern = r'\b([a-zA-Z_][a-zA-Z0-9_]*)\s*\('
            for match in re.finditer(pattern, content):
                name = match.group(1)
                if name not in ('print', 'len', 'range', 'enumerate', 'zip', 'map', 'filter'):
                    calls.add(name)
        return list(calls)[:20]


# ========================================================================
# AI Agent 代码记忆系统 - Namespace 精准定位版
# ========================================================================

class AICodeMemory:
    """
    AI Agent 代码记忆系统
    
    核心查询策略（按优先级，越来越模糊）：
    1. exact(namespace_path)     → 直接 namespace 路径命中
    2. symbol(name, repo)        → 符号表跳转定义
    3. browse_file(filepath)     → 浏览文件内所有符号
    4. search_in_repo(query)     → 限定在仓库内搜索
    5. fuzzy_global(query)       → 全局模糊搜索（兜底）
    """
    
    def __init__(self, cache_dir: str = "./ai_code_memory", max_memory: int = 2*1024*1024*1024):
        self.cache = open_cache(cache_dir, max_memory)
        self.parser = None
        
    # ========================================================================
    # 摄取代码（Ingest）
    # ========================================================================
    
    def ingest(self, repo_url: str, namespace: str = None) -> bool:
        """摄取代码仓库，建立 namespace 层级索引"""
        
        # 解析仓库名
        if repo_url.startswith('http'):
            parsed = urlparse(repo_url)
            parts = parsed.path.strip('/').split('/')
            owner, repo = parts[0], parts[1]
        else:
            repo = Path(repo_url).name
            owner = "local"
        
        if not namespace:
            namespace = f"/code/{owner}/{repo}"
        
        print(f"\n{'='*60}")
        print(f"AI Code Memory Ingest: {owner}/{repo}")
        print(f"Namespace: {namespace}")
        print(f"{'='*60}\n")
        
        tmpdir = tempfile.mkdtemp(prefix='ai_code_')
        try:
            if repo_url.startswith('http'):
                self._clone_repo(repo_url, tmpdir)
                repo_path = tmpdir
            else:
                repo_path = repo_url
            
            self.parser = CodeParser(repo_path)
            
            print("Phase 1: Parsing symbols with ctags...")
            tags = self.parser.run_ctags()
            print(f"  Found {len(tags)} raw tags")
            
            print("Phase 2: Computing scope ranges...")
            ranges = self.parser.extract_scope_range(tags)
            
            print("Phase 3: Building code chunks...")
            chunks = self._build_chunks(tags, ranges, namespace)
            print(f"  Created {len(chunks)} code chunks")
            
            print("Phase 4: Building symbol index...")
            symbols = self._build_symbol_index(tags, chunks, namespace)
            print(f"  Indexed {len(symbols)} symbols")
            
            print("Phase 5: Building reference graph...")
            refs = self._build_reference_graph(chunks, namespace)
            
            print("Phase 6: Storing to memory...")
            self._store_to_cache(chunks, symbols, refs, namespace, repo_path)
            
            print("Phase 7: Generating semantic vectors...")
            self._generate_vectors(chunks, namespace)
            
            self.cache.sync()
            print(f"\n{'='*60}")
            print(f"Ingest complete!")
            print(f"  Chunks: {len(chunks)}")
            print(f"  Symbols: {len(symbols)}")
            print(f"  Memory used: {self.cache.memory_used / 1024 / 1024:.1f} MB")
            print(f"{'='*60}\n")
            
            return True
            
        finally:
            if repo_url.startswith('http'):
                shutil.rmtree(tmpdir, ignore_errors=True)
    
    def _clone_repo(self, repo_url: str, tmpdir: str):
        cmd = ['git', 'clone', '--depth', '1', repo_url, tmpdir]
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            raise RuntimeError(f"Clone failed: {result.stderr}")
    
    def _build_chunks(self, tags, ranges, namespace):
        chunks = []
        seen = set()
        
        for tag in tags:
            if tag.get('_type') != 'tag':
                continue
            
            name = tag.get('name', '')
            filepath = tag.get('path', '')
            kind = tag.get('kind', 'unknown')
            line = tag.get('line', 0)
            signature = tag.get('signature', '')
            
            if not name or name.startswith('__') or ':' in name:
                continue
            
            chunk_id = hashlib.md5(f"{namespace}:{filepath}:{name}".encode()).hexdigest()[:16]
            if chunk_id in seen:
                continue
            seen.add(chunk_id)
            
            range_key = f"{filepath}:{name}"
            if range_key in ranges:
                start_line, end_line = ranges[range_key]
            else:
                start_line = line
                end_line = line + 30
            
            content = self.parser.parse_file_content(filepath, start_line, end_line)
            if not content.strip():
                continue
            
            language = self.parser.get_language(filepath)
            docstring = self.parser.extract_docstring(content, language)
            calls = self.parser.extract_calls(content, language)
            
            chunks.append({
                'id': chunk_id,
                'repo': namespace,
                'file': filepath,
                'name': name,
                'kind': kind,
                'line_start': start_line,
                'line_end': end_line,
                'language': language,
                'signature': signature,
                'content': content,
                'docstring': docstring,
                'calls': calls,
            })
        
        return chunks
    
    def _build_symbol_index(self, tags, chunks, namespace):
        symbols = {}
        chunk_map = {f"{c['file']}:{c['name']}": c['id'] for c in chunks}
        
        for tag in tags:
            if tag.get('_type') != 'tag':
                continue
            
            name = tag.get('name', '')
            if not name or name in symbols:
                continue
            
            filepath = tag.get('path', '')
            kind = tag.get('kind', 'unknown')
            line = tag.get('line', 0)
            signature = tag.get('signature', '')
            chunk_id = chunk_map.get(f"{filepath}:{name}", '')
            
            symbols[name] = {
                'name': name,
                'kind': kind,
                'file': filepath,
                'line': line,
                'signature': signature,
                'chunk_id': chunk_id,
            }
        
        return symbols
    
    def _build_reference_graph(self, chunks, namespace):
        refs = {}
        for chunk in chunks:
            for called_name in chunk['calls']:
                if called_name not in refs:
                    refs[called_name] = []
                refs[called_name].append(chunk['id'])
        return refs
    
    def _store_to_cache(self, chunks, symbols, refs, namespace, repo_path):
        # 1. 元数据
        self.cache.set_json(f"{namespace}/_meta/info", {
            "type": "code_repo",
            "chunk_count": len(chunks),
            "symbol_count": len(symbols),
            "languages": list(set(c['language'] for c in chunks)),
        }, 0)
        
        # 2. 代码片段（namespace 路径：/code/{repo}/chunks/{filepath}/{func_name}）
        for chunk in chunks:
            key = f"{namespace}/chunks/{chunk['file']}/{chunk['name']}"
            self.cache.set_json(key, {
                "id": chunk['id'],
                "kind": chunk['kind'],
                "line_start": chunk['line_start'],
                "line_end": chunk['line_end'],
                "language": chunk['language'],
                "signature": chunk['signature'],
                "docstring": chunk['docstring'],
                "content": chunk['content'],
                "calls": chunk['calls'],
                "called_by": refs.get(chunk['name'], []),
            }, 0)
        
        # 3. 符号索引
        symbol_groups = {}
        for name, sym in symbols.items():
            if name not in symbol_groups:
                symbol_groups[name] = []
            symbol_groups[name].append(sym)
        
        for name, entries in symbol_groups.items():
            self.cache.set_json(f"{namespace}/symbols/{name}", entries, 0)
        
        # 4. 文件索引
        file_chunks = {}
        for chunk in chunks:
            if chunk['file'] not in file_chunks:
                file_chunks[chunk['file']] = []
            file_chunks[chunk['file']].append({
                "name": chunk['name'],
                "kind": chunk['kind'],
                "line": chunk['line_start'],
                "chunk_id": chunk['id'],
            })
        
        for filepath, entries in file_chunks.items():
            self.cache.set_json(f"{namespace}/files/{filepath}", {
                "language": self.parser.get_language(filepath),
                "symbols": entries,
            }, 0)
        
        # 5. 引用关系
        for name, caller_ids in refs.items():
            self.cache.set_json(f"{namespace}/refs/{name}", caller_ids, 0)
    
    def _generate_vectors(self, chunks, namespace):
        """使用 C 端 ONNX embedder 生成向量（复用电子书实现）"""
        try:
            from mydb.onnx_embedder import OnnxEmbedder
            
            embedder = OnnxEmbedder()
            print(f"  Generating embeddings for {len(chunks)} chunks...")
            
            for i, chunk in enumerate(chunks):
                # 构建文本（签名 + 文档 + 代码前 5 行）
                text = f"{chunk['signature']}\n{chunk['docstring']}\n"
                text += '\n'.join(chunk['content'].split('\n')[:5])
                
                # C 端 ONNX 推理
                vector = embedder.encode(text)
                
                # 存储向量到 cache
                key = f"{namespace}/vectors/{chunk['id']}"
                value = json.dumps({
                    "chunk_id": chunk['id'],
                    "file": chunk['file'],
                    "name": chunk['name'],
                    "vector": vector,  # 384 维浮点数组
                })
                self.cache.set(key, value, 0)
                
                if (i + 1) % 100 == 0:
                    print(f"    Processed {i + 1}/{len(chunks)}")
            
            embedder.close()
            print(f"  Generated {len(chunks)} embeddings (dim={embedder.dim})")
            
        except ImportError:
            print("  ONNX embedder not available, skipping vector generation")
        except Exception as e:
            print(f"  Vector generation failed: {e}")
    
    # ========================================================================
    # AI Agent 查询 API - 按精准度排序
    # ========================================================================
    
    def exact(self, namespace_path: str) -> Optional[Dict]:
        """
        精确 namespace 路径查询 - O(1)
        
        路径格式:
          /code/{owner}/{repo}/chunks/{filepath}/{symbol_name}
          /code/{owner}/{repo}/files/{filepath}
          /code/{owner}/{repo}/symbols/{symbol_name}
        
        示例:
          exact("/code/redis/chunks/src/server.c/main")
          exact("/code/redis/files/src/server.c")
          exact("/code/redis/symbols/main")
        """
        value = self.cache.get(namespace_path)
        if not value:
            return None
        
        try:
            data = json.loads(value)
            data['_source'] = 'exact'
            data['_key'] = namespace_path
            return data
        except:
            return None
    
    def symbol(self, name: str, repo: str) -> Optional[Dict]:
        """
        符号跳转 - 直接命中符号表
        
        示例:
          symbol("cache_del_namespace", "/code/quqiufeng/my_db")
        """
        # 直接查找符号表
        key = f"{repo}/symbols/{name}"
        value = self.cache.get(key)
        
        if value:
            try:
                symbols = json.loads(value)
                if symbols:
                    sym = symbols[0]  # 取第一个定义
                    # 获取完整代码
                    chunk_key = f"{repo}/chunks/{sym['file']}/{name}"
                    chunk_data = self.exact(chunk_key)
                    if chunk_data:
                        return {
                            "name": name,
                            "file": sym['file'],
                            "line": sym['line'],
                            "kind": sym['kind'],
                            "signature": sym['signature'],
                            "content": chunk_data.get('content', ''),
                            "docstring": chunk_data.get('docstring', ''),
                            "_source": "symbol",
                            "_key": chunk_key,
                        }
            except:
                pass
        
        return None
    
    def browse_file(self, filepath: str, repo: str) -> List[Dict]:
        """
        浏览文件内所有符号 - 限定在单个文件
        
        示例:
          browse_file("src/server.c", "/code/redis/redis")
        """
        key = f"{repo}/files/{filepath}"
        data = self.exact(key)
        if not data:
            return []
        
        results = []
        for sym in data.get('symbols', []):
            chunk_key = f"{repo}/chunks/{filepath}/{sym['name']}"
            chunk_data = self.exact(chunk_key)
            if chunk_data:
                results.append({
                    "name": sym['name'],
                    "kind": sym['kind'],
                    "line": sym['line'],
                    "signature": chunk_data.get('signature', ''),
                    "content_preview": chunk_data.get('content', '')[:200],
                    "_source": "browse",
                    "_key": chunk_key,
                })
        
        return results
    
    def search_in_repo(self, query: str, repo: str, top_k: int = 5) -> List[Dict]:
        """
        限定在仓库内的搜索 - 利用 namespace 过滤
        
        策略:
        1. 先尝试精确匹配符号名
        2. 在 repo 范围内关键词搜索
        3. 在 repo 范围内前缀搜索
        
        示例:
          search_in_repo("LRU eviction", "/code/redis/redis")
          search_in_repo("cache_del_namespace", "/code/quqiufeng/my_db")
        """
        results = []
        seen = set()
        
        # 策略 1: 如果是符号名，直接跳转
        sym_data = self.symbol(query, repo)
        if sym_data:
            results.append(sym_data)
            seen.add(sym_data.get('_key', ''))
        
        # 策略 2: 关键词搜索（限定在 repo namespace）
        keywords = self._extract_keywords(query)
        for kw in keywords:
            # 使用 tag 搜索
            tag_results = self.cache.search_tag(kw, max_results=top_k * 2)
            for r in tag_results:
                key = r['key']
                # 限定在 repo namespace 内
                if not key.startswith(repo):
                    continue
                if key in seen:
                    continue
                
                try:
                    data = json.loads(r['value'])
                    if 'content' in data:
                        results.append({
                            "name": data.get('name', ''),
                            "file": data.get('file', ''),
                            "line": data.get('line_start', 0),
                            "kind": data.get('kind', ''),
                            "signature": data.get('signature', ''),
                            "content": data['content'],
                            "docstring": data.get('docstring', ''),
                            "score": r['score'],
                            "_source": "tag",
                            "_key": key,
                        })
                        seen.add(key)
                except:
                    pass
        
        # 策略 3: 前缀搜索（适合 cache_xxx 这类前缀明确的查询）
        if query.startswith(('cache_', 'hnsw_', 'db_')):
            prefix_results = self.cache.search_prefix(f"{repo}/chunks/", max_results=top_k * 2)
            for r in prefix_results:
                key = r['key']
                # 检查是否匹配查询
                if query not in key:
                    continue
                if key in seen:
                    continue
                
                try:
                    data = json.loads(r['value'])
                    if 'content' in data:
                        results.append({
                            "name": data.get('name', ''),
                            "file": data.get('file', ''),
                            "line": data.get('line_start', 0),
                            "kind": data.get('kind', ''),
                            "signature": data.get('signature', ''),
                            "content": data['content'],
                            "docstring": data.get('docstring', ''),
                            "score": 1.0,
                            "_source": "prefix",
                            "_key": key,
                        })
                        seen.add(key)
                except:
                    pass
        
        return results[:top_k]
    
    def fuzzy_global(self, query: str, top_k: int = 5) -> List[Dict]:
        """
        全局模糊搜索 - 最后的兜底手段
        
        仅在以下情况使用：
        - 用户不知道仓库名
        - 查询非常模糊（如 "hash table implementation"）
        
        示例:
          fuzzy_global("hash resize algorithm")
        """
        results = []
        
        # 模糊搜索
        fuzzy_results = self.cache.search_fuzzy(query, max_results=top_k * 2)
        
        for r in fuzzy_results:
            key = r['key']
            try:
                data = json.loads(r['value'])
                if 'content' in data:
                    results.append({
                        "name": data.get('name', ''),
                        "file": data.get('file', ''),
                        "line": data.get('line_start', 0),
                        "kind": data.get('kind', ''),
                        "signature": data.get('signature', ''),
                        "content": data['content'],
                        "docstring": data.get('docstring', ''),
                        "score": r['score'],
                        "_source": "fuzzy",
                        "_key": key,
                    })
            except:
                pass
        
        return results[:top_k]
    
    # ========================================================================
    # 高级查询（组合策略）
    # ========================================================================
    
    def ask(self, query: str, repo: str = None, top_k: int = 5) -> List[Dict]:
        """
        智能查询 - 自动选择最佳策略
        
        策略选择逻辑:
        1. 如果 query 包含 / → 尝试精确路径
        2. 如果 query 是 snake_case / camelCase → 尝试符号跳转
        3. 如果 repo 指定了 → 在 repo 内搜索
        4. 否则 → 全局模糊搜索
        
        示例:
          ask("cache_del_namespace")                    → 符号跳转
          ask("src/server.c:42")                        → 精确路径
          ask("LRU eviction", "/code/redis/redis")      → 仓库内搜索
          ask("hash table implementation")              → 全局模糊搜索
        """
        results = []
        
        # 策略 1: 精确路径（包含 / 且不以空格分隔）
        if '/' in query and ' ' not in query:
            # 可能是 namespace 路径
            if query.startswith('/code/'):
                data = self.exact(query)
                if data:
                    return [data]
            
            # 可能是 filepath:line 格式
            if ':' in query:
                parts = query.rsplit(':', 1)
                if parts[1].isdigit():
                    # filepath:line 格式，尝试定位到最近的符号
                    if repo:
                        file_results = self.browse_file(parts[0], repo)
                        line = int(parts[1])
                        for r in file_results:
                            if abs(r['line'] - line) < 20:
                                results.append(r)
                        if results:
                            return results[:top_k]
        
        # 策略 2: 符号名（snake_case / camelCase，无空格）
        if ' ' not in query and repo:
            data = self.symbol(query, repo)
            if data:
                return [data]
        
        # 策略 3: 限定仓库搜索
        if repo:
            results = self.search_in_repo(query, repo, top_k)
            if results:
                return results
        
        # 策略 4: 全局模糊搜索（兜底）
        return self.fuzzy_global(query, top_k)
    
    def context(self, symbol_name: str, repo: str, radius: int = 3) -> Dict:
        """
        获取符号的上下文（调用者 + 被调用者）
        
        示例:
          context("cache_del_namespace", "/code/quqiufeng/my_db")
        """
        # 获取主符号
        main = self.symbol(symbol_name, repo)
        if not main:
            return {"error": f"Symbol '{symbol_name}' not found"}
        
        result = {
            "symbol": main,
            "callers": [],
            "callees": [],
        }
        
        # 获取引用关系
        refs_key = f"{repo}/refs/{symbol_name}"
        refs_data = self.cache.get(refs_key)
        if refs_data:
            try:
                caller_ids = json.loads(refs_data)
                for chunk_id in caller_ids[:radius]:
                    # 反向查找
                    caller = self._find_chunk_by_id(chunk_id, repo)
                    if caller:
                        result['callers'].append(caller)
            except:
                pass
        
        # 获取被调用者（从主符号的 calls 列表）
        chunk_key = f"{repo}/chunks/{main['file']}/{symbol_name}"
        chunk_data = self.exact(chunk_key)
        if chunk_data and 'calls' in chunk_data:
            for called_name in chunk_data['calls'][:radius]:
                callee = self.symbol(called_name, repo)
                if callee:
                    result['callees'].append(callee)
        
        return result
    
    def _find_chunk_by_id(self, chunk_id: str, repo: str) -> Optional[Dict]:
        """通过 chunk_id 反向查找"""
        # 在 repo 范围内搜索
        results = self.cache.search_prefix(f"{repo}/chunks/", max_results=1000)
        for r in results:
            try:
                data = json.loads(r['value'])
                if data.get('id') == chunk_id:
                    return {
                        "name": data.get('name', ''),
                        "file": data.get('file', ''),
                        "line": data.get('line_start', 0),
                        "kind": data.get('kind', ''),
                        "content_preview": data.get('content', '')[:200],
                    }
            except:
                pass
        return None
    
    def _extract_keywords(self, question: str) -> List[str]:
        """提取技术关键词"""
        keywords = []
        pattern = r'\b[a-zA-Z_][a-zA-Z0-9_]*(?:_[a-zA-Z0-9_]+)*\b'
        for match in re.finditer(pattern, question):
            word = match.group()
            if len(word) > 3 and word not in ('this', 'that', 'what', 'when', 'where', 'how'):
                keywords.append(word)
        return keywords[:5]
    
    def list_repos(self) -> List[str]:
        """列出所有已摄取的仓库"""
        results = []
        # 搜索 /code/ 下的直接子目录
        prefix_results = self.cache.search_prefix("/code/", max_results=1000)
        seen = set()
        for r in prefix_results:
            key = r['key']
            # 提取 /code/owner/repo 部分
            parts = key.split('/')
            if len(parts) >= 4 and parts[1] == 'code':
                repo_path = '/'.join(parts[:4])  # /code/owner/repo
                if repo_path not in seen:
                    seen.add(repo_path)
                    results.append(repo_path)
        return results
    
    def stats(self) -> Dict:
        """统计信息"""
        return {
            "entries": self.cache.count,
            "memory_used_mb": self.cache.memory_used / 1024 / 1024,
            "memory_max_mb": self.cache.memory_max / 1024 / 1024,
            "repos": self.list_repos(),
        }
    
    def close(self):
        self.cache.close()


# ========================================================================
# CLI 接口
# ========================================================================

def main():
    import argparse
    
    parser = argparse.ArgumentParser(
        description='AI Agent Code Memory System - Namespace Edition',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
用法示例:
  # 摄取仓库
  %(prog)s ingest https://github.com/redis/redis

  # 精确查询（最快）
  %(prog)s exact /code/redis/redis/chunks/src/server.c/main

  # 符号跳转
  %(prog)s symbol cache_del_namespace --repo /code/quqiufeng/my_db

  # 浏览文件
  %(prog)s browse src/server.c --repo /code/redis/redis

  # 仓库内搜索
  %(prog)s search "LRU eviction" --repo /code/redis/redis

  # 智能查询（自动选择策略）
  %(prog)s ask "cache_del_namespace" --repo /code/quqiufeng/my_db
  %(prog)s ask "hash resize algorithm"
        """
    )
    
    parser.add_argument('--cache-dir', default='./ai_code_memory', help='Cache directory')
    parser.add_argument('--repo', default='/code', help='Repository namespace')
    
    subparsers = parser.add_subparsers(dest='command', help='Commands')
    
    # ingest
    ingest_parser = subparsers.add_parser('ingest', help='Ingest a repository')
    ingest_parser.add_argument('repo_url', help='GitHub URL or local path')
    ingest_parser.add_argument('--namespace', '-n', help='Override namespace')
    
    # exact
    exact_parser = subparsers.add_parser('exact', help='Exact namespace path lookup')
    exact_parser.add_argument('path', help='Namespace path like /code/redis/chunks/src/server.c/main')
    
    # symbol
    symbol_parser = subparsers.add_parser('symbol', help='Jump to symbol definition')
    symbol_parser.add_argument('name', help='Symbol name')
    
    # browse
    browse_parser = subparsers.add_parser('browse', help='Browse file symbols')
    browse_parser.add_argument('filepath', help='File path like src/server.c')
    
    # search
    search_parser = subparsers.add_parser('search', help='Search within repo')
    search_parser.add_argument('query', help='Search query')
    search_parser.add_argument('--top-k', '-k', type=int, default=5)
    
    # ask
    ask_parser = subparsers.add_parser('ask', help='Smart query (auto strategy)')
    ask_parser.add_argument('query', help='Query string')
    ask_parser.add_argument('--top-k', '-k', type=int, default=5)
    
    # context
    context_parser = subparsers.add_parser('context', help='Get symbol context')
    context_parser.add_argument('symbol', help='Symbol name')
    
    # list
    subparsers.add_parser('list', help='List all repositories')
    
    # stats
    subparsers.add_parser('stats', help='Show statistics')
    
    args = parser.parse_args()
    
    memory = AICodeMemory(args.cache_dir)
    
    try:
        if args.command == 'ingest':
            success = memory.ingest(args.repo_url, args.namespace)
            sys.exit(0 if success else 1)
        
        elif args.command == 'exact':
            result = memory.exact(args.path)
            if result:
                print(f"\nExact match: {args.path}\n")
                _print_chunk(result)
            else:
                print(f"Not found: {args.path}")
        
        elif args.command == 'symbol':
            result = memory.symbol(args.name, args.repo)
            if result:
                print(f"\nSymbol: {args.name}\n")
                _print_symbol(result)
            else:
                print(f"Symbol not found: {args.name}")
        
        elif args.command == 'browse':
            results = memory.browse_file(args.filepath, args.repo)
            print(f"\nFile: {args.filepath} ({len(results)} symbols)\n")
            for i, r in enumerate(results, 1):
                print(f"  {i}. [{r['kind']}] {r['name']} (line {r['line']})")
                if r.get('signature'):
                    print(f"     {r['signature']}")
        
        elif args.command == 'search':
            results = memory.search_in_repo(args.query, args.repo, args.top_k)
            print(f"\nSearch: '{args.query}' in {args.repo}")
            print(f"Found {len(results)} results:\n")
            for i, r in enumerate(results, 1):
                print(f"[{i}] {r['file']}:{r['line']} ({r['_source']})")
                print(f"    {r['name']} {r.get('signature', '')}")
                print(f"    {r['content'][:300]}...")
                print()
        
        elif args.command == 'ask':
            results = memory.ask(args.query, args.repo, args.top_k)
            print(f"\nAsk: '{args.query}'")
            if args.repo:
                print(f"Repo: {args.repo}")
            print(f"Found {len(results)} results:\n")
            for i, r in enumerate(results, 1):
                source = r.get('_source', 'unknown')
                key = r.get('_key', '')
                print(f"[{i}] {source} | {key}")
                if 'content' in r:
                    lines = r['content'].split('\n')[:8]
                    for line in lines:
                        print(f"    {line}")
                print()
        
        elif args.command == 'context':
            result = memory.context(args.symbol, args.repo)
            if 'error' in result:
                print(f"Error: {result['error']}")
            else:
                print(f"\nSymbol: {result['symbol']['name']}\n")
                print(f"File: {result['symbol']['file']}:{result['symbol']['line']}")
                print(f"Content:\n{result['symbol']['content'][:500]}...")
                
                if result['callers']:
                    print(f"\nCalled by ({len(result['callers'])}):")
                    for c in result['callers']:
                        print(f"  - {c['name']} @ {c['file']}:{c['line']}")
                
                if result['callees']:
                    print(f"\nCalls ({len(result['callees'])}):")
                    for c in result['callees']:
                        print(f"  - {c['name']} @ {c['file']}:{c['line']}")
        
        elif args.command == 'list':
            repos = memory.list_repos()
            print(f"\nRepositories ({len(repos)}):\n")
            for repo in repos:
                print(f"  {repo}")
        
        elif args.command == 'stats':
            stats = memory.stats()
            print(f"\nStatistics:")
            print(f"  Entries: {stats['entries']}")
            print(f"  Memory: {stats['memory_used_mb']:.1f} / {stats['memory_max_mb']:.1f} MB")
            print(f"  Repos: {len(stats['repos'])}")
        
        else:
            parser.print_help()
    
    finally:
        memory.close()


def _print_chunk(data: Dict):
    """打印代码片段"""
    print(f"Name: {data.get('name', 'N/A')}")
    print(f"Kind: {data.get('kind', 'N/A')}")
    print(f"File: {data.get('file', 'N/A')}:{data.get('line_start', 0)}-{data.get('line_end', 0)}")
    if data.get('signature'):
        print(f"Signature: {data['signature']}")
    if data.get('docstring'):
        print(f"Doc:\n{data['docstring']}")
    print(f"\nContent:\n{'='*60}")
    print(data.get('content', 'N/A'))
    print('='*60)


def _print_symbol(data: Dict):
    """打印符号信息"""
    print(f"File: {data['file']}:{data['line']}")
    print(f"Kind: {data['kind']}")
    if data.get('signature'):
        print(f"Signature: {data['signature']}")
    if data.get('docstring'):
        print(f"Doc:\n{data['docstring']}")
    print(f"\nContent:\n{'='*60}")
    print(data.get('content', 'N/A')[:1000])
    print('='*60)


if __name__ == '__main__':
    main()
