#!/usr/bin/env python3
"""
agent_query.py — AI-friendly structured query interface for KV Cache

Usage:
    ./agent_query.sh <query> [--repo <namespace>] [--type <exact|symbol|search|context>]
    
Examples:
    # Exact namespace lookup
    ./agent_query.sh /code/nginx/symbols/ngx_palloc
    
    # Symbol context (function + callers + callees + dataflow)
    ./agent_query.sh ngx_palloc --repo /code/nginx --type context
    
    # Search within repo
    ./agent_query.sh "memory pool" --repo /code/nginx --type search
    
    # AI-friendly JSON output (all queries return structured JSON)
    ./agent_query.sh ngx_array_init --repo /code/nginx --type context --json

Output format (structured JSON):
    {
        "query": "ngx_palloc",
        "type": "context",
        "repo": "/code/nginx",
        "results": [...],
        "context": {
            "symbol": {...},
            "callers": [...],
            "callees": [...],
            "dataflow": [...]
        },
        "timing_ms": 45
    }

Design:
    - All output is machine-readable JSON
    - Context queries aggregate related information
    - Repository-scoped by default (faster, more relevant)
    - Falls back to global search if repo not specified
"""

import os
import sys
import json
import time
import argparse
from typing import List, Dict, Optional, Any

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from mydb.cache import open_cache

DEFAULT_CACHE_DIR = os.environ.get('MYDB_CACHE_DIR', './ai_code_memory')


class AgentQuery:
    """AI-friendly query interface over KV Cache"""
    
    def __init__(self, cache_dir: str = DEFAULT_CACHE_DIR):
        self.cache = open_cache(cache_dir, 2 * 1024 * 1024 * 1024)
    
    def query(self, query_str: str, repo: Optional[str] = None, 
              query_type: str = "auto") -> Dict[str, Any]:
        """
        Execute a query and return structured results.
        
        Args:
            query_str: The query string
            repo: Repository namespace (e.g., /code/nginx)
            query_type: Query strategy (auto, exact, symbol, search, context)
        """
        start_time = time.time()
        
        # Auto-detect query type
        if query_type == "auto":
            query_type = self._detect_query_type(query_str, repo)
        
        # Execute query
        if query_type == "exact":
            results = self._exact_query(query_str)
        elif query_type == "symbol":
            results = self._symbol_query(query_str, repo)
        elif query_type == "context":
            results = self._context_query(query_str, repo)
        elif query_type == "search":
            results = self._search_query(query_str, repo)
        else:
            results = self._smart_query(query_str, repo)
        
        elapsed_ms = int((time.time() - start_time) * 1000)
        
        return {
            "query": query_str,
            "type": query_type,
            "repo": repo,
            "results": results.get("results", []),
            "context": results.get("context", {}),
            "stats": results.get("stats", {}),
            "timing_ms": elapsed_ms,
        }
    
    def _detect_query_type(self, query: str, repo: Optional[str]) -> str:
        """Auto-detect the best query strategy"""
        # Exact path: starts with /code/
        if query.startswith('/code/'):
            return "exact"
        
        # Symbol name: no spaces, looks like identifier
        if ' ' not in query and self._is_identifier(query):
            if repo:
                return "context"  # Get full context if repo specified
            return "symbol"
        
        # Natural language: has spaces
        if ' ' in query:
            return "search"
        
        return "smart"
    
    def _is_identifier(self, s: str) -> bool:
        """Check if string looks like a code identifier"""
        if not s:
            return False
        # snake_case or camelCase or PascalCase
        return s[0].isalpha() or s[0] == '_' and all(c.isalnum() or c == '_' for c in s)
    
    def _exact_query(self, path: str) -> Dict:
        """Exact namespace path lookup"""
        value = self.cache.get(path)
        if not value:
            return {"results": []}
        
        try:
            data = json.loads(value)
            if isinstance(data, list):
                # Symbol entries are stored as arrays
                return {"results": [{"_key": path, "_source": "exact", "entries": data}]}
            elif isinstance(data, dict):
                data["_key"] = path
                data["_source"] = "exact"
                return {"results": [data]}
            else:
                return {"results": [{"_key": path, "_source": "exact", "value": data}]}
        except:
            return {"results": [{"_key": path, "value": value}]}
    
    def _symbol_query(self, name: str, repo: Optional[str]) -> Dict:
        """Symbol lookup by name"""
        results = []
        
        if repo:
            # Try specific repo first
            key = f"{repo}/symbols/{name}"
            value = self.cache.get(key)
            if value:
                try:
                    symbols = json.loads(value)
                    for sym in symbols:
                        # Get full chunk
                        chunk_key = f"{repo}/chunks/{sym['file']}/{name}"
                        chunk_data = self._get_json(chunk_key)
                        if chunk_data:
                            results.append({
                                **chunk_data,
                                "_source": "symbol",
                                "_key": chunk_key,
                            })
                        else:
                            results.append({
                                **sym,
                                "_source": "symbol",
                                "_key": key,
                            })
                except:
                    pass
        
        # If no results, search all repos
        if not results:
            prefix_results = self.cache.search_prefix(f"/code/", max_results=1000)
            seen_repos = set()
            for r in prefix_results:
                key = r['key']
                parts = key.split('/')
                if len(parts) >= 4 and parts[1] == 'code':
                    repo_path = '/'.join(parts[:4])
                    if repo_path not in seen_repos:
                        seen_repos.add(repo_path)
                        sym_key = f"{repo_path}/symbols/{name}"
                        sym_data = self._get_json(sym_key)
                        if sym_data:
                            for sym in sym_data:
                                chunk_key = f"{repo_path}/chunks/{sym['file']}/{name}"
                                chunk_data = self._get_json(chunk_key)
                                if chunk_data:
                                    results.append({
                                        **chunk_data,
                                        "_source": "symbol",
                                        "_key": chunk_key,
                                        "_repo": repo_path,
                                    })
        
        return {"results": results}
    
    def _context_query(self, name: str, repo: str) -> Dict:
        """Get full context: symbol + callers + callees + dataflow"""
        # Get symbol
        symbol_result = self._symbol_query(name, repo)
        symbol = symbol_result["results"][0] if symbol_result["results"] else None
        
        # Get callers
        callers = self._get_callers(name, repo)
        
        # Get callees
        callees = self._get_callees(name, repo)
        
        # Get call sites
        call_sites = self._get_call_sites(name, repo)
        
        # Get dataflow
        dataflow = self._get_dataflow(name, repo)
        
        return {
            "results": [symbol] if symbol else [],
            "context": {
                "symbol": symbol,
                "callers": callers,
                "callees": callees,
                "call_sites": call_sites,
                "dataflow": dataflow,
            },
            "stats": {
                "caller_count": len(callers),
                "callee_count": len(callees),
                "call_site_count": len(call_sites),
                "dataflow_entries": len(dataflow),
            }
        }
    
    def _search_query(self, query: str, repo: Optional[str]) -> Dict:
        """Search within repo or globally"""
        results = []
        seen = set()
        
        # Strategy 1: Tag search (for technical keywords like function names)
        keywords = self._extract_keywords(query)
        for kw in keywords:
            tag_results = self.cache.search_tag(kw, max_results=20)
            for r in tag_results:
                key = r['key']
                if repo and not key.startswith(repo):
                    continue
                if 'chunks/' not in key:  # Only include code chunks
                    continue
                if key in seen:
                    continue
                seen.add(key)
                
                try:
                    data = json.loads(r['value'])
                    if 'content' in data:
                        results.append({
                            "name": data.get('name', ''),
                            "file": data.get('file', ''),
                            "line": data.get('line_start', 0),
                            "content": data['content'][:500],
                            "score": r['score'] + 0.1,  # Boost tag matches
                            "_source": "tag",
                            "_key": key,
                        })
                except:
                    pass
        
        # Strategy 2: Prefix search (for namespace queries like cache_*)
        if not results and any(c in query for c in ['_', '-']):
            prefix_results = self.cache.search_prefix(f"{repo}/chunks/" if repo else "/code/", max_results=100)
            for r in prefix_results:
                key = r['key']
                if not any(kw in key for kw in keywords):
                    continue
                if key in seen:
                    continue
                seen.add(key)
                
                try:
                    data = json.loads(r['value'])
                    if 'content' in data:
                        results.append({
                            "name": data.get('name', ''),
                            "file": data.get('file', ''),
                            "line": data.get('line_start', 0),
                            "content": data['content'][:500],
                            "score": 0.8,
                            "_source": "prefix",
                            "_key": key,
                        })
                except:
                    pass
        
        # Strategy 3: Scan all chunks for keyword match (last resort)
        if not results and repo:
            # Scan all chunks in repo for content match
            all_chunks = self.cache.search_prefix(f"{repo}/chunks/", max_results=500)
            query_lower = query.lower()
            for r in all_chunks:
                key = r['key']
                if key in seen:
                    continue
                
                try:
                    data = json.loads(r['value'])
                    content = data.get('content', '').lower()
                    if any(kw.lower() in content for kw in keywords) or query_lower in content:
                        results.append({
                            "name": data.get('name', ''),
                            "file": data.get('file', ''),
                            "line": data.get('line_start', 0),
                            "content": data['content'][:500],
                            "score": 0.5,
                            "_source": "content_scan",
                            "_key": key,
                        })
                        seen.add(key)
                        if len(results) >= 10:
                            break
                except:
                    pass
        
        # Sort by score
        results.sort(key=lambda x: x.get('score', 0), reverse=True)
        
        return {"results": results[:10]}
    
    def _smart_query(self, query: str, repo: Optional[str]) -> Dict:
        """Smart query: try multiple strategies"""
        # Try exact first
        if query.startswith('/code/'):
            return self._exact_query(query)
        
        # Try symbol
        if self._is_identifier(query):
            result = self._symbol_query(query, repo)
            if result["results"]:
                return result
        
        # Fall back to search
        return self._search_query(query, repo)
    
    def _get_callers(self, name: str, repo: str) -> List[Dict]:
        """Get callers of a function"""
        key = f"{repo}/callers/{name}"
        data = self._get_json(key)
        if not data:
            return []
        return data.get('callers', [])
    
    def _get_callees(self, name: str, repo: str) -> List[Dict]:
        """Get callees of a function"""
        key = f"{repo}/callees/{name}"
        data = self._get_json(key)
        if not data:
            return []
        return data.get('callees', [])
    
    def _get_call_sites(self, name: str, repo: str) -> List[Dict]:
        """Get call sites of a function"""
        key = f"{repo}/call_sites/{name}"
        data = self._get_json(key)
        if not data:
            return []
        return data.get('sites', [])
    
    def _get_dataflow(self, name: str, repo: str) -> List[Dict]:
        """Get dataflow information for a variable/function"""
        results = []
        
        # Try variable dataflow
        key = f"{repo}/dataflow/vars/{name}"
        data = self._get_json(key)
        if data:
            results.append({"type": "variable", "data": data})
        
        # Try function dataflow
        key = f"{repo}/dataflow/func/{name}"
        data = self._get_json(key)
        if data:
            results.append({"type": "function", "data": data})
        
        return results
    
    def _get_json(self, key: str) -> Optional[Dict]:
        """Get and parse JSON value from cache"""
        value = self.cache.get(key)
        if not value:
            return None
        try:
            return json.loads(value)
        except:
            return None
    
    def _extract_keywords(self, query: str) -> List[str]:
        """Extract technical keywords from query"""
        import re
        keywords = []
        pattern = r'\b[a-zA-Z_][a-zA-Z0-9_]*(?:_[a-zA-Z0-9_]+)*\b'
        for match in re.finditer(pattern, query):
            word = match.group()
            if len(word) > 3 and word.lower() not in ('this', 'that', 'what', 'when', 'where', 'how', 'the', 'and', 'for', 'with'):
                keywords.append(word)
        return keywords[:5]
    
    def close(self):
        self.cache.close()


def main():
    parser = argparse.ArgumentParser(
        description='AI-friendly structured query interface for KV Cache',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
    # Exact lookup
    %(prog)s /code/nginx/symbols/ngx_palloc
    
    # Symbol with full context
    %(prog)s ngx_palloc --repo /code/nginx --type context
    
    # Search
    %(prog)s "memory pool allocation" --repo /code/nginx --type search
    
    # AI-friendly JSON (default)
    %(prog)s ngx_array_init --repo /code/nginx --type context
        """
    )
    
    parser.add_argument('query', help='Query string or namespace path')
    parser.add_argument('--repo', '-r', help='Repository namespace (e.g., /code/nginx)')
    parser.add_argument('--type', '-t', default='auto',
                        choices=['auto', 'exact', 'symbol', 'context', 'search'],
                        help='Query type (default: auto-detect)')
    parser.add_argument('--cache-dir', '-d', default=DEFAULT_CACHE_DIR,
                        help=f'Cache directory (default: {DEFAULT_CACHE_DIR})')
    parser.add_argument('--pretty', '-p', action='store_true',
                        help='Pretty-print JSON output')
    
    args = parser.parse_args()
    
    agent = AgentQuery(cache_dir=args.cache_dir)
    
    try:
        result = agent.query(
            query_str=args.query,
            repo=args.repo,
            query_type=args.type,
        )
        
        # Build JSON string before closing cache (to avoid log pollution)
        json_output = json.dumps(result, indent=2 if args.pretty else None, ensure_ascii=False)
        
    except KeyboardInterrupt:
        agent.close()
        sys.exit(130)
    
    # Print JSON before closing cache to avoid cache log messages in output
    print(json_output, flush=True)
    
    # Close cache — suppress its stdout log spam
    import os
    _fd = os.dup(1)
    _null = os.open(os.devnull, os.O_WRONLY)
    os.dup2(_null, 1)
    try:
        agent.close()
    finally:
        os.dup2(_fd, 1)
        os.close(_null)
        os.close(_fd)


if __name__ == '__main__':
    main()
