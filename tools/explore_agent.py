#!/usr/bin/env python3
"""
AI Agent 探索演示 - stable-diffusion.cpp

使用自然语言查询 + 向量索引探索代码库
"""

import sys
import os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from tools.agent import CodeMemoryAgent


def explore_with_agent():
    agent = CodeMemoryAgent('./ai_code_memory')
    namespace = '/code/local/stable-diffusion.cpp'
    
    print("🤖 AI Agent 探索 stable-diffusion.cpp")
    print("="*70)
    
    # 自然语言查询示例
    queries = [
        "generate image from text prompt",
        "upscale image resolution",
        "control image generation with edge detection",
        "load diffusion model weights",
        "convert image to different format",
        "video generation from text",
        "memory allocation for tensors",
        "CUDA kernel for sampling",
        "save generated image to file",
        "parse model checkpoint",
    ]
    
    for query in queries:
        print(f"\n📌 Query: \"{query}\"")
        print("-" * 50)
        
        # 语义搜索
        results = agent.semantic_search(namespace, query, max_results=5)
        
        if isinstance(results, dict) and 'error' in results:
            print(f"  Error: {results['error']}")
            continue
        
        print(f"  Top matches:")
        for i, r in enumerate(results[:5], 1):
            print(f"    {i}. {r['name']} (score: {r['score']})")
            
            # 获取详细信息
            # Try to find the chunk
            chunk = agent.get_chunk(namespace, r['name'].split('_')[0] if '_' in r['name'] else r['name'], r['name'].rsplit('_', 1)[0] if '_' in r['name'] else r['name'])
            if chunk:
                file = chunk.get('file', 'N/A')
                line = chunk.get('line_start', 'N/A')
                sig = chunk.get('signature', '')[:80]
                print(f"       📍 {file}:{line}")
                if sig:
                    print(f"       📎 {sig}")
            else:
                # Try symbol lookup
                sym = agent.query_symbol(namespace, r['name'].rsplit('_', 1)[0] if '_' in r['name'] else r['name'])
                if sym and len(sym) > 0:
                    loc = sym[0]
                    print(f"       📍 {loc.get('file', 'N/A')}:{loc.get('line', 'N/A')}")
    
    # 特定功能探索
    print("\n\n" + "="*70)
    print("🔍 深度探索：图像生成核心 API")
    print("="*70)
    
    core_apis = ['generate_image', 'new_sd_ctx', 'sd_ctx_params_init', 'preprocess_canny']
    for api in core_apis:
        print(f"\n📎 {api}")
        chunk = agent.get_chunk(namespace, 'include/stable-diffusion.h', api)
        if chunk:
            print(f"   File: {chunk.get('file', 'N/A')}:{chunk.get('line_start', 'N/A')}")
            print(f"   Signature: {chunk.get('signature', 'N/A')[:100]}")
        
        # 调用关系
        callers = agent.get_callers(namespace, api)
        if callers:
            print(f"   Called by: {callers.get('callers', [])[:3]}")
        
        callees = agent.get_callees(namespace, api)
        if callees:
            print(f"   Calls: {callees.get('callees', [])[:3]}")
    
    # 文件结构概览
    print("\n\n" + "="*70)
    print("📁 项目文件结构")
    print("="*70)
    files = agent.list_files(namespace)
    print(f"Total files: {len(files)}")
    
    # 按目录分组
    dirs = {}
    for f in files:
        dir_name = f.split('/')[0] if '/' in f else 'root'
        if dir_name not in dirs:
            dirs[dir_name] = []
        dirs[dir_name].append(f)
    
    for dir_name, dir_files in sorted(dirs.items()):
        print(f"\n  📂 {dir_name}/ ({len(dir_files)} files)")
        for f in dir_files[:5]:
            print(f"    • {os.path.basename(f)}")
        if len(dir_files) > 5:
            print(f"    ... and {len(dir_files) - 5} more")


if __name__ == '__main__':
    explore_with_agent()
