#!/usr/bin/env python3
"""
通用 AI Agent 代码记忆系统接口

支持任意已 ingest 的项目，不依赖特定项目结构。
"""

import os
import sys
import json

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from mydb.cache import open_cache


class CodeMemoryAgent:
    """通用代码记忆 Agent"""
    
    def __init__(self, cache_dir='./ai_code_memory'):
        self.cache_dir = cache_dir
        self.cache = open_cache(cache_dir)
    
    def list_repos(self):
        """列出所有已索引的仓库"""
        repos = set()
        for key in self.cache.keys():
            if key.startswith('/code/'):
                parts = key.split('/')
                if len(parts) >= 4:
                    # /code/{owner}/{repo}/...
                    repo = f"/{parts[1]}/{parts[2]}/{parts[3]}"
                    repos.add(repo)
        return sorted(repos)
    
    def get_repo_info(self, namespace):
        """获取仓库基本信息"""
        meta_key = f"{namespace}/_meta/info"
        data = self.cache.get(meta_key)
        if data:
            return json.loads(data)
        return None
    
    def query_symbol(self, namespace, name):
        """查询符号定义位置"""
        key = f"{namespace}/symbols/{name}"
        data = self.cache.get(key)
        if data:
            return json.loads(data)
        return None
    
    def get_chunk(self, namespace, filepath, name):
        """获取代码片段"""
        key = f"{namespace}/chunks/{filepath}/{name}"
        data = self.cache.get(key)
        if data:
            return json.loads(data)
        return None
    
    def search_symbols(self, namespace, query, max_results=20):
        """在仓库中搜索符号"""
        results = []
        query_lower = query.lower()
        prefix = f"{namespace}/symbols/"
        
        for key in self.cache.keys():
            if not key.startswith(prefix):
                continue
            name = key[len(prefix):]
            if query_lower in name.lower():
                results.append(name)
            if len(results) >= max_results:
                break
        
        return results
    
    def search_code(self, namespace, query, max_results=20):
        """搜索代码内容"""
        results = []
        query_lower = query.lower()
        prefix = f"{namespace}/chunks/"
        
        for key in self.cache.keys():
            if not key.startswith(prefix):
                continue
            try:
                data = json.loads(self.cache.get(key))
                text = f"{data.get('signature', '')} {data.get('docstring', '')} {data.get('content', '')}"
                if query_lower in text.lower():
                    results.append({
                        'name': key.split('/')[-1],
                        'file': data.get('file'),
                        'line': data.get('line_start'),
                        'signature': data.get('signature')
                    })
                if len(results) >= max_results:
                    break
            except:
                pass
        
        return results
    
    def get_callers(self, namespace, func_name):
        """获取调用者（谁调用了这个函数）"""
        key = f"{namespace}/callers/{func_name}"
        data = self.cache.get(key)
        if data:
            return json.loads(data)
        return None
    
    def get_callees(self, namespace, func_name):
        """获取被调用者（这个函数调用了谁）"""
        key = f"{namespace}/callees/{func_name}"
        data = self.cache.get(key)
        if data:
            return json.loads(data)
        return None
    
    def list_files(self, namespace):
        """列出仓库中的所有文件"""
        files = set()
        prefix = f"{namespace}/chunks/"
        
        for key in self.cache.keys():
            if not key.startswith(prefix):
                continue
            # key format: /namespace/chunks/{filepath}/{name}
            rel = key[len(prefix):]
            if '/' in rel:
                filepath = rel.rsplit('/', 1)[0]
                files.add(filepath)
        
        return sorted(files)
    
    def get_file_symbols(self, namespace, filepath):
        """获取文件中的所有符号"""
        key = f"{namespace}/files/{filepath}"
        data = self.cache.get(key)
        if data:
            return json.loads(data)
        return None
    
    def semantic_search(self, namespace, query, max_results=10):
        """语义搜索（读取二进制向量文件）"""
        import struct
        import os
        
        # Build binary file path
        safe_ns = namespace.replace('/', '_').strip('_')
        vec_dir = os.path.join(self.cache_dir, 'vectors')
        vec_file = os.path.join(vec_dir, f"{safe_ns}.bin")
        idx_file = os.path.join(vec_dir, f"{safe_ns}.idx")
        
        if not os.path.exists(vec_file) or not os.path.exists(idx_file):
            return {'error': f'Vector index not found. Run vector generator first.'}
        
        try:
            from mydb.onnx_embedder import OnnxEmbedder
            embedder = OnnxEmbedder(
                model_path='models/all-mpnet-base-v2/model.onnx',
                vocab_path='models/all-mpnet-base-v2/vocab.txt',
                max_seq_length=128,
                dim=768
            )
            query_vec = embedder.encode(query)
            
            # Load index mapping name -> offset
            with open(idx_file, 'r') as f:
                index = json.load(f)
            
            # Read vectors and compute similarity
            results = []
            with open(vec_file, 'rb') as f:
                # Read header
                count, dim = struct.unpack('II', f.read(8))
                
                for name, offset in index.items():
                    f.seek(offset)
                    vec = struct.unpack(f'{dim}f', f.read(dim * 4))
                    
                    # Cosine similarity
                    dot = sum(a*b for a,b in zip(query_vec, vec))
                    norm_q = sum(a*a for a in query_vec) ** 0.5
                    norm_v = sum(a*a for a in vec) ** 0.5
                    sim = dot / (norm_q * norm_v) if norm_q > 0 and norm_v > 0 else 0
                    
                    results.append((sim, name))
            
            results.sort(reverse=True)
            return [{'name': name, 'score': f'{sim:.3f}'} for sim, name in results[:max_results]]
            
        except Exception as e:
            return {'error': f'Semantic search failed: {e}'}


def main():
    agent = CodeMemoryAgent()
    
    print("🤖 Code Memory Agent")
    print("="*60)
    
    # 列出仓库
    repos = agent.list_repos()
    print(f"\n📦 Indexed Repositories ({len(repos)}):")
    for repo in repos:
        info = agent.get_repo_info(repo)
        if info:
            print(f"  • {repo}")
            print(f"    Chunks: {info.get('chunk_count', 'N/A')}")
            print(f"    Symbols: {info.get('symbol_count', 'N/A')}")
        else:
            print(f"  • {repo} (no meta)")
    
    if not repos:
        print("  No repositories found. Run: python3 tools/ai_code_memory.py ingest <path>")
        return
    
    # 示例查询
    namespace = repos[0]
    print(f"\n📌 Demo queries for {namespace}:")
    
    # 1. 搜索符号
    print("\n1. Search symbols containing 'init':")
    symbols = agent.search_symbols(namespace, 'init', 5)
    for s in symbols[:5]:
        print(f"   • {s}")
    
    # 2. 搜索代码
    print("\n2. Search code containing 'memory':")
    code_results = agent.search_code(namespace, 'memory', 5)
    for r in code_results[:5]:
        print(f"   • {r['name']} in {r['file']}:{r['line']}")
    
    # 3. 列出文件
    print(f"\n3. Files in {namespace}:")
    files = agent.list_files(namespace)
    print(f"   Total: {len(files)} files")
    for f in files[:5]:
        print(f"   • {f}")
    
    print("\n💡 Usage:")
    print("   from tools.agent import CodeMemoryAgent")
    print("   agent = CodeMemoryAgent()")
    print("   agent.query_symbol('/code/local/myrepo', 'main')")
    print("   agent.search_code('/code/local/myrepo', 'error handling')")


if __name__ == '__main__':
    main()
