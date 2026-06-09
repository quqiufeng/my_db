#!/usr/bin/env python3
"""
AI Agent 编码助手 - 实时第三方源码查询

功能：
1. 解析当前编码上下文的 import/include 语句
2. 自动匹配 memory 中已索引的第三方库
3. 查询调用的函数/类的具体实现
4. 返回使用示例和注意事项

用法：
    from tools.coding_assistant import CodingAssistant
    
    assistant = CodingAssistant("/memory")
    
    # AI 正在写代码，提供上下文
    context = '''
    #include <hnswlib/hnswalg.h>
    
    int main() {
        hnswlib::HNSWIndex<float> index;
        index.addPoint(data, label);
    }
    '''
    
    # 查询第三方库详情
    info = assistant.query_context(context, "/code/redis/redis")
    # → 返回 hnswlib::HNSWIndex 的定义、addPoint 的实现、使用示例
"""

import os
import sys
import json
import re
from typing import List, Dict, Optional, Tuple

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from mydb.cache import open_cache
from tools.ai_code_memory import AICodeMemory


class CodingAssistant:
    """
    AI Agent 编码助手
    
    让 AI 在编码时能实时了解第三方库的具体实现
    """
    
    def __init__(self, cache_dir: str = "/memory"):
        self.memory = AICodeMemory(cache_dir)
        self._repo_cache = {}  # 缓存已识别的仓库
        
    def analyze_imports(self, code: str, language: str = None) -> Dict[str, List[str]]:
        """
        解析代码中的 import/include 语句
        
        Returns:
            {
                "libraries": ["hnswlib", "onnxruntime", ...],  # 第三方库名
                "symbols": ["HNSWIndex", "addPoint", ...],     # 引用的符号
                "headers": ["hnswlib/hnswalg.h", ...],         # 头文件路径
            }
        """
        if not language:
            language = self._detect_language(code)
        
        imports = {
            "libraries": [],
            "symbols": [],
            "headers": [],
            "modules": [],
        }
        
        if language in ("c", "cpp", "c++", "objc"):
            # C/C++: #include <xxx.h> 或 #include "xxx.h"
            include_pattern = r'#include\s*["<]([^">]+)[">]'
            for match in re.finditer(include_pattern, code):
                header = match.group(1)
                imports["headers"].append(header)
                # 提取库名（如 hnswlib/hnswalg.h → hnswlib）
                lib_name = header.split('/')[0]
                if lib_name and lib_name not in imports["libraries"]:
                    imports["libraries"].append(lib_name)
            
            # 提取函数调用（如 new_sd_ctx(params)）
            symbol_pattern = r'\b([a-zA-Z_]\w*)\s*\('
            for match in re.finditer(symbol_pattern, code):
                symbol = match.group(1)
                # 排除关键字
                if symbol not in ('if', 'while', 'for', 'switch', 'return', 'sizeof', 'typeof', 'main'):
                    if symbol not in imports["symbols"]:
                        imports["symbols"].append(symbol)
            
            # 也提取 :: 命名空间引用（如 hnswlib::HNSWIndex）
            ns_pattern = r'\b([a-zA-Z_]\w*(?:::[a-zA-Z_]\w*)+)\s*(?:\(|\{|;|\.)'
            for match in re.finditer(ns_pattern, code):
                symbol = match.group(1)
                if symbol not in imports["symbols"]:
                    imports["symbols"].append(symbol)
        
        elif language == "python":
            # Python: import xxx 或 from xxx import yyy
            # import xxx
            for match in re.finditer(r'^import\s+([\w.]+)', code, re.MULTILINE):
                module = match.group(1).split('.')[0]
                if module not in imports["modules"]:
                    imports["modules"].append(module)
            
            # from xxx import yyy
            for match in re.finditer(r'^from\s+([\w.]+)\s+import', code, re.MULTILINE):
                module = match.group(1).split('.')[0]
                if module not in imports["modules"]:
                    imports["modules"].append(module)
                # 提取导入的符号
                line = match.group(0)
                symbols_part = code[match.end():].split('\n')[0]
                for sym in re.findall(r'([a-zA-Z_]\w*)', symbols_part):
                    if sym not in ('import', 'as', 'from'):
                        imports["symbols"].append(sym)
        
        elif language == "go":
            # Go: import "xxx" 或 import ("xxx" "yyy")
            for match in re.finditer(r'import\s+(?:\(\s*)?"([^"]+)"', code):
                pkg = match.group(1)
                pkg_name = pkg.split('/')[-1]
                imports["libraries"].append(pkg_name)
                imports["modules"].append(pkg)
        
        elif language in ("rust", "rs"):
            # Rust: use xxx::yyy;
            for match in re.finditer(r'use\s+([\w:]+)', code):
                module = match.group(1).split('::')[0]
                if module not in imports["modules"]:
                    imports["modules"].append(module)
        
        elif language in ("javascript", "typescript", "js", "ts"):
            # JS/TS: import { x } from 'xxx' 或 require('xxx')
            for match in re.finditer(r'(?:import\s+.*?from\s+|require\s*\(\s*)[\'"]([^\'"]+)[\'"]', code):
                module = match.group(1)
                if not module.startswith('.'):
                    imports["libraries"].append(module)
        
        return imports
    
    def _detect_language(self, code: str) -> str:
        """根据代码特征检测语言"""
        if '#include' in code or ('::' in code and '{' in code):
            if '#include <iostream>' in code or 'std::' in code:
                return 'cpp'
            return 'c'
        elif re.search(r'^import\s+|^from\s+', code, re.MULTILINE):
            return 'python'
        elif 'package ' in code and 'func ' in code:
            return 'go'
        elif 'use ' in code and '::' in code:
            return 'rust'
        elif 'import ' in code and ('from ' in code or 'require(' in code):
            return 'javascript'
        return 'unknown'
    
    def find_repos_for_imports(self, imports: Dict) -> List[str]:
        """
        根据 import 的库名，在 memory 中找到对应的仓库
        
        Returns:
            ["/code/hnswlib/hnswlib", "/code/microsoft/onnxruntime", ...]
        """
        repos = self.memory.list_repos()
        matched = []
        
        # 从 imports 中提取可能的库名
        search_terms = set()
        search_terms.update(imports.get("libraries", []))
        search_terms.update(imports.get("modules", []))
        search_terms.update(imports.get("headers", []))
        
        # 也从头文件中提取核心名称（如 stable-diffusion.h → stable-diffusion）
        for header in imports.get("headers", []):
            base = header.split('/')[-1].split('.')[0]  # 去掉路径和扩展名
            if base:
                search_terms.add(base)
        
        # 匹配仓库名
        for repo in repos:
            # repo 格式: /code/owner/name
            parts = repo.split('/')
            if len(parts) >= 3:
                repo_name = parts[-1].lower()
                owner = parts[-2].lower()
                
                for term in search_terms:
                    term_lower = term.lower()
                    # 直接匹配仓库名
                    if repo_name in term_lower or term_lower in repo_name:
                        if repo not in matched:
                            matched.append(repo)
                            break
                    # 匹配 owner
                    if owner in term_lower or term_lower in owner:
                        if repo not in matched:
                            matched.append(repo)
                            break
                    # 模糊匹配：去掉连字符和点号后比较
                    repo_normalized = repo_name.replace('-', '').replace('_', '')
                    term_normalized = term_lower.replace('-', '').replace('_', '').replace('.', '')
                    if repo_normalized in term_normalized or term_normalized in repo_normalized:
                        if repo not in matched:
                            matched.append(repo)
                            break
        
        return matched
    
    def query_symbol(self, symbol_name: str, repo: str) -> Optional[Dict]:
        """
        查询符号的详细信息（轻量级：只返回签名和位置，不返回实现）
        
        Returns:
            {
                "definition": {...},      # 定义位置（文件、行号、签名）
                "signatures": [...],       # 所有重载签名
            }
        """
        import json
        
        # 从符号表查询
        key = f"{repo}/symbols/{symbol_name}"
        value = self.memory.cache.get(key)
        
        if not value:
            return None
        
        try:
            symbols = json.loads(value)
        except:
            return None
        
        if not symbols:
            return None
        
        # 取第一个作为定义（通常是主要定义）
        primary = symbols[0]
        
        result = {
            "definition": {
                "name": symbol_name,
                "kind": primary.get("kind", "unknown"),
                "file": primary.get("file", ""),
                "line": primary.get("line", 0),
                "signature": primary.get("signature", ""),
            },
            "signatures": [s.get("signature", "") for s in symbols if s.get("signature")],
            "overloads": symbols if len(symbols) > 1 else [],
        }
        
        return result
    
    def query_context(self, code: str, current_repo: str = None, top_k: int = 3) -> Dict:
        """
        分析编码上下文，返回相关的第三方库信息
        
        Args:
            code: 当前代码上下文
            current_repo: 当前项目的 repo（避免查询自己）
            top_k: 最多返回多少个相关库的信息
        
        Returns:
            {
                "imports": {...},              # 解析到的 import
                "matched_repos": ["/code/..."], # 匹配的仓库
                "symbol_info": {
                    "symbol_name": {
                        "definition": {...},
                        "examples": [...],
                    }
                }
            }
        """
        # 1. 解析 imports
        imports = self.analyze_imports(code)
        
        result = {
            "imports": imports,
            "matched_repos": [],
            "symbol_info": {},
        }
        
        # 2. 找到相关的仓库
        matched_repos = self.find_repos_for_imports(imports)
        
        # 过滤掉当前仓库（避免查询自己）
        if current_repo:
            matched_repos = [r for r in matched_repos if r != current_repo]
        
        result["matched_repos"] = matched_repos
        
        # 3. 查询符号信息
        for repo in matched_repos[:top_k]:  # 最多 top_k 个仓库
            for symbol in imports.get("symbols", []):
                # 提取简单符号名（如 hnswlib::HNSWIndex → HNSWIndex）
                simple_name = symbol.split("::")[-1]
                
                info = self.query_symbol(simple_name, repo)
                if info:
                    key = f"{repo.split('/')[-1]}::{simple_name}"
                    result["symbol_info"][key] = info
        
        return result
    
    def get_quick_help(self, symbol_name: str, repo_hint: str = None) -> str:
        """
        获取快速帮助文本（轻量级：只返回签名和位置）
        
        Args:
            symbol_name: 符号名，如 "hnswlib::HNSWIndex" 或 "addPoint"
            repo_hint: 仓库提示，如 "/code/redis/redis"
        
        Returns:
            Markdown 格式的帮助文本
        """
        # 如果没有 repo_hint，搜索所有仓库
        repos = [repo_hint] if repo_hint else self.memory.list_repos()
        
        for repo in repos:
            info = self.query_symbol(symbol_name, repo)
            if info and info.get("definition"):
                d = info["definition"]
                
                help_text = f"""### {symbol_name}

**定义**: `{d['file']}:{d['line']}`
**类型**: {d['kind']}
"""
                
                if d.get("signature"):
                    help_text += f"**签名**: `{d['signature']}`\n"
                
                # 显示重载（如果有）
                if info.get("overloads"):
                    help_text += f"\n**重载 ({len(info['overloads'])})**:\n"
                    for overload in info["overloads"][:3]:  # 最多显示3个
                        sig = overload.get("signature", "")
                        file = overload.get("file", "")
                        line = overload.get("line", 0)
                        if sig:
                            help_text += f"- `{sig}` @ `{file}:{line}`\n"
                
                # 提示 AI 去查看源码
                repo_path = self._get_repo_path(repo)
                if repo_path and d.get("file"):
                    full_path = os.path.join(repo_path, d['file'])
                    help_text += f"\n**查看实现**: `{full_path}:{d['line']}`\n"
                
                return help_text
        
        return f"Symbol '{symbol_name}' not found in memory."
    
    def _get_repo_path(self, repo: str) -> str:
        """获取仓库的本地路径"""
        import json
        meta_key = f"{repo}/_meta/info"
        value = self.memory.cache.get(meta_key)
        if value:
            try:
                meta = json.loads(value)
                return meta.get("repo_path", "")
            except:
                pass
        return ""
    
    def close(self):
        self.memory.close()


# ========================================================================
# CLI 接口
# ========================================================================

def main():
    import argparse
    
    parser = argparse.ArgumentParser(
        description='AI Coding Assistant - Query third-party code from memory',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
用法示例:
  # 分析代码上下文
  %(prog)s analyze '
  #include <hnswlib/hnswalg.h>
  hnswlib::HNSWIndex<float> index;
  index.addPoint(data, label);
  ' --repo /code/my_project

  # 查询具体符号
  %(prog)s lookup addPoint --repo /code/hnswlib/hnswlib

  # 获取快速帮助（适合插入 AI 提示词）
  %(prog)s help "std::vector::push_back" --repo /code/gcc/gcc
        """
    )
    
    parser.add_argument('--cache-dir', default='/memory', help='Cache directory')
    parser.add_argument('--repo', default=None, help='Current repository namespace')
    
    subparsers = parser.add_subparsers(dest='command', help='Commands')
    
    # analyze
    analyze_parser = subparsers.add_parser('analyze', help='Analyze code context')
    analyze_parser.add_argument('code', help='Code snippet')
    analyze_parser.add_argument('--top-k', '-k', type=int, default=3)
    analyze_parser.add_argument('--repo', default=None, help='Current repository namespace')
    
    # lookup
    lookup_parser = subparsers.add_parser('lookup', help='Lookup symbol details')
    lookup_parser.add_argument('symbol', help='Symbol name')
    lookup_parser.add_argument('--repo', default=None, help='Repository namespace')
    
    # help
    help_parser = subparsers.add_parser('help', help='Get quick help text')
    help_parser.add_argument('symbol', help='Symbol name')
    help_parser.add_argument('--repo', default=None, help='Repository namespace')
    
    args = parser.parse_args()
    
    assistant = CodingAssistant(args.cache_dir)
    
    try:
        if args.command == 'analyze':
            result = assistant.query_context(args.code, args.repo, args.top_k)
            
            print(f"\n解析到的 Imports:")
            for key, values in result["imports"].items():
                if values:
                    print(f"  {key}: {values}")
            
            print(f"\n匹配的仓库:")
            for repo in result["matched_repos"]:
                print(f"  {repo}")
            
            print(f"\n符号详情:")
            for sym_name, info in result["symbol_info"].items():
                print(f"\n  {sym_name}:")
                if info.get("definition"):
                    d = info["definition"]
                    print(f"    定义: {d['file']}:{d['line']}")
                    if d.get("signature"):
                        print(f"    签名: {d['signature']}")
                
                if info.get("examples"):
                    print(f"    示例:")
                    for ex in info["examples"][:2]:
                        print(f"      - {ex['caller_name']} @ {ex['caller_file']}:{ex['caller_line']}")
        
        elif args.command == 'lookup':
            # 如果没有指定 repo，搜索所有仓库
            repos = [args.repo] if args.repo else assistant.memory.list_repos()
            
            for repo in repos:
                info = assistant.query_symbol(args.symbol, repo)
                if info:
                    print(f"\n{'='*60}")
                    print(f"Repository: {repo}")
                    print(f"{'='*60}")
                    
                    if info.get("definition"):
                        d = info["definition"]
                        print(f"\n定义: {d['file']}:{d['line']}")
                        print(f"类型: {d['kind']}")
                        if d.get("signature"):
                            print(f"签名: {d['signature']}")
                        if d.get("content"):
                            print(f"\n源码:\n{d['content'][:1000]}")
                    
                    if info.get("examples"):
                        print(f"\n使用示例 ({len(info['examples'])}):")
                        for i, ex in enumerate(info["examples"][:3], 1):
                            print(f"\n  {i}. {ex['caller_name']}")
                    
                    break  # 找到第一个就停
            else:
                print(f"Symbol '{args.symbol}' not found")
        
        elif args.command == 'help':
            help_text = assistant.get_quick_help(args.symbol, args.repo)
            print(help_text)
        
        else:
            parser.print_help()
    
    finally:
        assistant.close()


if __name__ == '__main__':
    main()
