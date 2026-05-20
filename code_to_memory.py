#!/usr/bin/env python3
"""
code_to_memory.py — Bridge: Import analysis results into KV Cache

Usage:
    ./code_to_memory.sh <analysis_dir> [namespace] [--cache-dir <dir>]
    
Examples:
    # Import Linux kernel mm/ analysis
    ./code_to_memory.sh ./linux_subsystems/mm_cache /code/linux/mm
    
    # Import nginx analysis
    ./code_to_memory.sh ./nginx_cache /code/nginx
    
    # Import with custom cache location
    ./code_to_memory.sh ./linux_subsystems/mm_cache /code/linux/mm --cache-dir ./ai_memory

Input format (from analysis pipeline):
    - chunks_meta.jsonl  : Chunk metadata (name, file, line, content, etc.)
    - call_graph.json    : Caller/callee relationships with call sites
    - dataflow.json      : Variable dataflow with field-level tracking
    - vectors.bin        : Binary vectors (stored by reference, not content)

Output:
    Populates KV Cache with namespace hierarchy:
    /code/{project}/_meta/info          — Project metadata
    /code/{project}/chunks/{file}/{name} — Code chunks with content
    /code/{project}/symbols/{name}       — Symbol index
    /code/{project}/callers/{name}       — Who calls this function
    /code/{project}/callees/{name}       — What this function calls
    /code/{project}/dataflow/{var}       — Variable dataflow paths
    /code/{project}/files/{filepath}     — File symbol list
    /code/{project}/vectors/path         — Vector file reference

Design decisions:
    1. Reuses existing analysis output (no re-parsing)
    2. Stores vectors by file path reference (not inline — too large)
    3. Maintains same namespace schema as ai_code_memory.py for compatibility
    4. Skips low-quality entries (headers, empty content)
"""

import os
import sys
import json
import time
import argparse
from pathlib import Path
from typing import List, Dict, Optional

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from mydb.cache import open_cache

# Configuration
CACHE_MAX_VALUE_LEN = 1024 * 1024  # 1MB max value
SYNC_INTERVAL = 5000  # Sync every 5000 entries
MEANINGFUL_KINDS = {'function', 'method', 'class', 'struct', 'namespace', 
                    'macro', 'typedef', 'enum', 'interface', 'prototype'}


def log(msg: str):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}")


class AnalysisImporter:
    """Imports analysis results into KV Cache memory"""
    
    def __init__(self, cache_dir: str = "./ai_code_memory", max_memory: int = 2*1024*1024*1024):
        self.cache = open_cache(cache_dir, max_memory)
        self.total_keys = 0
    
    def safe_set_json(self, key: str, value: dict, ttl: int = 0) -> bool:
        """Store JSON value in cache with size check"""
        json_str = json.dumps(value, ensure_ascii=False)
        if len(json_str.encode('utf-8')) >= CACHE_MAX_VALUE_LEN:
            log(f"[WARN] Value too large, skipping: {key}")
            return False
        
        result = self.cache.set_json(key, value, ttl)
        if result:
            self.total_keys += 1
            if self.total_keys % SYNC_INTERVAL == 0:
                self.cache.sync()
                log(f"Synced at {self.total_keys} keys")
        return result
    
    def import_analysis(self, analysis_dir: str, namespace: str) -> bool:
        """
        Import all analysis results from a directory into KV Cache.
        
        Args:
            analysis_dir: Directory containing analysis output files
            namespace: Target namespace (e.g., /code/linux/mm)
        """
        analysis_path = Path(analysis_dir)
        if not analysis_path.exists():
            log(f"ERROR: Analysis directory not found: {analysis_dir}")
            return False
        
        log(f"Importing analysis into namespace: {namespace}")
        log(f"Source directory: {analysis_dir}")
        
        # Detect available files
        chunks_file = analysis_path / "chunks_meta.jsonl"
        callgraph_file = analysis_path / "call_graph.json"
        dataflow_file = analysis_path / "dataflow.json"
        vectors_file = analysis_path / "vectors.bin"
        
        files_found = []
        if chunks_file.exists(): files_found.append("chunks_meta.jsonl")
        if callgraph_file.exists(): files_found.append("call_graph.json")
        if dataflow_file.exists(): files_found.append("dataflow.json")
        if vectors_file.exists(): files_found.append("vectors.bin")
        
        log(f"Found: {', '.join(files_found) if files_found else 'NONE'}")
        
        # Phase 1: Import chunks (core)
        chunks_count = 0
        symbols_count = 0
        if chunks_file.exists():
            chunks_count, symbols_count = self._import_chunks(chunks_file, namespace)
        
        # Phase 2: Import call graph
        callers_count = 0
        callees_count = 0
        if callgraph_file.exists():
            callers_count, callees_count = self._import_callgraph(callgraph_file, namespace)
        
        # Phase 3: Import dataflow
        dataflow_count = 0
        if dataflow_file.exists():
            dataflow_count = self._import_dataflow(dataflow_file, namespace)
        
        # Phase 4: Register vector file reference
        if vectors_file.exists():
            self._register_vectors(vectors_file, namespace)
        
        # Phase 5: Store metadata
        self._store_metadata(namespace, analysis_dir, {
            'chunks': chunks_count,
            'symbols': symbols_count,
            'callers': callers_count,
            'callees': callees_count,
            'dataflow_vars': dataflow_count,
        })
        
        # Final sync
        self.cache.sync()
        
        log(f"Import complete!")
        log(f"  Chunks: {chunks_count}")
        log(f"  Symbols: {symbols_count}")
        log(f"  Callers: {callers_count}")
        log(f"  Callees: {callees_count}")
        log(f"  Dataflow vars: {dataflow_count}")
        log(f"  Total keys: {self.total_keys}")
        log(f"  Memory used: {self.cache.memory_used / 1024 / 1024:.1f} MB")
        
        return True
    
    def _import_chunks(self, chunks_file: Path, namespace: str) -> tuple:
        """Import chunks from chunks_meta.jsonl"""
        log("Phase 1: Importing chunks...")
        
        chunks = []
        line_count = 0
        
        with open(chunks_file, 'r', encoding='utf-8') as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    chunk = json.loads(line)
                    line_count += 1
                    
                    # Skip low-quality entries
                    kind = chunk.get('kind', '')
                    if kind == 'header':
                        continue
                    
                    # Extract key fields
                    name = chunk.get('name', '')
                    filepath = chunk.get('file', '')
                    if not name or not filepath:
                        continue
                    
                    chunks.append(chunk)
                except json.JSONDecodeError:
                    continue
        
        log(f"  Parsed {line_count} lines, {len(chunks)} valid chunks")
        
        # Normalize filepaths: remove leading '/' to avoid double slashes in keys
        for chunk in chunks:
            chunk['file'] = chunk['file'].lstrip('/')
        
        # Build symbol groups and file groups
        symbol_groups = {}
        file_groups = {}
        
        for chunk in chunks:
            name = chunk['name']
            filepath = chunk['file']
            
            # Symbol grouping
            if name not in symbol_groups:
                symbol_groups[name] = []
            symbol_groups[name].append({
                "name": name,
                "kind": chunk.get('kind', 'unknown'),
                "file": filepath,
                "line": chunk.get('line_start', 0),
                "signature": chunk.get('signature', ''),
                "language": chunk.get('language', 'unknown'),
            })
            
            # File grouping
            if filepath not in file_groups:
                file_groups[filepath] = []
            file_groups[filepath].append({
                "name": name,
                "kind": chunk.get('kind', 'unknown'),
                "line": chunk.get('line_start', 0),
                "signature": chunk.get('signature', ''),
            })
        
        # Store chunks (only meaningful kinds)
        stored_chunks = 0
        for chunk in chunks:
            if chunk.get('kind') not in MEANINGFUL_KINDS:
                continue
            
            filepath = chunk['file']
            name = chunk['name']
            
            # Build content: signature + docstring + code
            lines = chunk.get('content', '').split('\n')
            code_text = '\n'.join(lines[:50])  # First 50 lines
            
            text = ""
            if chunk.get('signature'):
                text += f"{chunk['signature']}\n"
            if chunk.get('docstring'):
                text += f"{chunk['docstring']}\n"
            text += code_text
            
            # Build tags for searchability
            tags = [name, chunk.get('kind', ''), chunk.get('language', '')]
            # Add keywords from signature and content
            if chunk.get('signature'):
                tags.extend(self._extract_tags_from_text(chunk['signature']))
            if chunk.get('content'):
                tags.extend(self._extract_tags_from_text(chunk['content'][:500]))
            
            chunk_data = {
                "id": f"{namespace}:{filepath}:{name}",
                "name": name,
                "file": filepath,
                "kind": chunk.get('kind', ''),
                "line_start": chunk.get('line_start', 0),
                "line_end": chunk.get('line_end', 0),
                "language": chunk.get('language', ''),
                "signature": chunk.get('signature', ''),
                "docstring": chunk.get('docstring', ''),
                "content": text[:16000],  # Cap at 16KB
                "tags": list(set(t for t in tags if t and len(t) > 2))[:20],  # Deduplicate and limit
            }
            
            key = f"{namespace}/chunks/{filepath}/{name}"
            if self.safe_set_json(key, chunk_data):
                stored_chunks += 1
        
        # Store symbols
        for name, entries in symbol_groups.items():
            key = f"{namespace}/symbols/{name}"
            self.safe_set_json(key, entries)
        
        # Store file indexes
        for filepath, syms in file_groups.items():
            key = f"{namespace}/files/{filepath}"
            self.safe_set_json(key, {
                "path": filepath,
                "symbols": syms,
            })
        
        log(f"  Stored {stored_chunks} chunks, {len(symbol_groups)} symbols, {len(file_groups)} files")
        return stored_chunks, len(symbol_groups)
    
    def _import_callgraph(self, callgraph_file: Path, namespace: str) -> tuple:
        """Import call graph from call_graph.json"""
        log("Phase 2: Importing call graph...")
        
        with open(callgraph_file, 'r', encoding='utf-8') as f:
            data = json.load(f)
        
        # call_graph.json format: {"func_name": {"calls": [{"function": ..., "file": ..., "line": ..., "arguments": [...]}]}}
        # Build callers and callees maps
        callers_map = {}  # func -> [callers]
        callees_map = {}  # func -> [callees]
        call_sites_map = {}  # func -> [{file, line, args}]
        
        for caller_name, caller_data in data.items():
            if not isinstance(caller_data, dict):
                continue
            
            calls = caller_data.get('calls', [])
            for call in calls:
                callee_name = call.get('function', '')
                if not callee_name:
                    continue
                
                call_site = {
                    'file': call.get('file', ''),
                    'line': call.get('line', 0),
                    'args': ', '.join(call.get('arguments', [])) if isinstance(call.get('arguments'), list) else str(call.get('arguments', '')),
                }
                
                # Build callers map (who calls callee)
                if callee_name not in callers_map:
                    callers_map[callee_name] = []
                callers_map[callee_name].append({
                    "name": caller_name,
                    "file": call_site['file'],
                    "line": call_site['line'],
                    "args": call_site['args'],
                })
                
                # Build callees map (who does caller call)
                if caller_name not in callees_map:
                    callees_map[caller_name] = []
                callees_map[caller_name].append({
                    "name": callee_name,
                    "file": call_site['file'],
                    "line": call_site['line'],
                    "args": call_site['args'],
                })
                
                # Build call sites map
                if callee_name not in call_sites_map:
                    call_sites_map[callee_name] = []
                call_sites_map[callee_name].append({
                    "caller": caller_name,
                    "file": call_site['file'],
                    "line": call_site['line'],
                    "args": call_site['args'],
                })
        
        # Store callers
        callers_stored = 0
        for func_name, caller_list in callers_map.items():
            unique_callers = list({c['name']: c for c in caller_list}.values())[:50]
            key = f"{namespace}/callers/{func_name}"
            self.safe_set_json(key, {
                "count": len(caller_list),
                "callers": unique_callers,
            })
            callers_stored += 1
        
        # Store callees
        callees_stored = 0
        for func_name, callee_list in callees_map.items():
            unique_callees = list({c['name']: c for c in callee_list}.values())[:50]
            key = f"{namespace}/callees/{func_name}"
            self.safe_set_json(key, {
                "count": len(callee_list),
                "callees": unique_callees,
            })
            callees_stored += 1
        
        # Store call sites
        for func_name, sites in call_sites_map.items():
            key = f"{namespace}/call_sites/{func_name}"
            self.safe_set_json(key, {
                "count": len(sites),
                "sites": sites[:100],  # Cap at 100 sites
            })
        
        log(f"  Stored {callers_stored} caller entries, {callees_stored} callee entries")
        return callers_stored, callees_stored
    
    def _import_dataflow(self, dataflow_file: Path, namespace: str) -> int:
        """Import dataflow from dataflow.json"""
        log("Phase 3: Importing dataflow...")
        
        try:
            with open(dataflow_file, 'r', encoding='utf-8') as f:
                content = f.read()
            data = json.loads(content)
        except json.JSONDecodeError as e:
            log(f"  WARNING: dataflow.json has JSON errors: {e}")
            log("  Attempting recovery with relaxed parsing...")
            try:
                data = self._parse_dataflow_relaxed(dataflow_file)
            except Exception as e2:
                log(f"  ERROR: Failed to parse dataflow.json: {e2}")
                log("  Skipping dataflow import")
                return 0
        
        # dataflow.json format: {"var_name": {"occurrences": [{"file": ..., "func": ..., "line": ..., "type": ..., "context": ...}]}}
        
        var_groups = {}
        
        for var_name, var_data in data.items():
            if not isinstance(var_data, dict):
                continue
            
            occurrences = var_data.get('occurrences', [])
            if not occurrences:
                continue
            
            var_groups[var_name] = occurrences
            
            # Store variable occurrences
            key = f"{namespace}/dataflow/vars/{var_name}"
            self.safe_set_json(key, {
                "count": len(occurrences),
                "occurrences": occurrences[:100],  # Cap at 100
            })
        
        # Group by function for cross-function analysis
        func_vars = {}
        for var_name, occurrences in var_groups.items():
            for occ in occurrences:
                func_name = occ.get('func', 'unknown')
                if func_name not in func_vars:
                    func_vars[func_name] = []
                func_vars[func_name].append({
                    "variable": var_name,
                    "file": occ.get('file', ''),
                    "line": occ.get('line', 0),
                    "type": occ.get('type', ''),
                    "context": occ.get('context', '')[:500],  # Truncate context
                })
        
        # Store function-level variable usage
        for func_name, vars_in_func in func_vars.items():
            key = f"{namespace}/dataflow/func/{func_name}"
            self.safe_set_json(key, {
                "function": func_name,
                "variable_count": len(vars_in_func),
                "variables": vars_in_func[:100],
            })
        
        total_vars = len(var_groups)
        log(f"  Stored {total_vars} variables, {len(func_vars)} function profiles")
        return total_vars
    
    def _register_vectors(self, vectors_file: Path, namespace: str):
        """Register vector file reference (not the actual vectors)"""
        log("Phase 4: Registering vector file...")
        
        abs_path = str(vectors_file.resolve())
        file_size = vectors_file.stat().st_size
        
        key = f"{namespace}/vectors/path"
        self.safe_set_json(key, {
            "file": abs_path,
            "size_bytes": file_size,
            "type": "float32",
            "description": "Vector file reference — use vector_search tool to query",
        })
        
        log(f"  Vector file: {abs_path} ({file_size / 1024 / 1024:.1f} MB)")
    
    def _store_metadata(self, namespace: str, source_dir: str, stats: dict):
        """Store project metadata"""
        key = f"{namespace}/_meta/info"
        self.safe_set_json(key, {
            "type": "code_repo",
            "source_dir": source_dir,
            "imported_at": time.strftime('%Y-%m-%d %H:%M:%S'),
            "chunks": stats.get('chunks', 0),
            "symbols": stats.get('symbols', 0),
            "callers": stats.get('callers', 0),
            "callees": stats.get('callees', 0),
            "dataflow_vars": stats.get('dataflow_vars', 0),
        })
    
    def _extract_tags_from_text(self, text: str) -> list:
        """Extract searchable tags from text (signature or content)"""
        import re
        # Extract snake_case and camelCase identifiers
        words = re.findall(r'\b[a-zA-Z_][a-zA-Z0-9_]*(?:_[a-zA-Z0-9_]+)*\b', text)
        # Filter out common C keywords and short words
        stopwords = {'if', 'for', 'while', 'return', 'void', 'int', 'char', 'static', 
                     'const', 'struct', 'union', 'enum', 'typedef', 'sizeof', 'NULL',
                     'else', 'do', 'switch', 'case', 'break', 'continue', 'goto',
                     'extern', 'inline', 'register', 'volatile', 'signed', 'unsigned',
                     'short', 'long', 'float', 'double', 'auto'}
        return [w for w in words if len(w) > 3 and w.lower() not in stopwords][:10]
    
    def _parse_dataflow_relaxed(self, dataflow_file: Path) -> dict:
        """Relaxed parser for dataflow.json with unescaped quotes"""
        import re
        
        result = {}
        current_var = None
        current_occurrences = []
        
        with open(dataflow_file, 'r', encoding='utf-8', errors='replace') as f:
            for line in f:
                line = line.strip()
                if not line or line in ['{', '}']:
                    continue
                
                # Check for variable name (e.g., "var_name": {)
                var_match = re.match(r'"([^"]+)":\s*\{', line)
                if var_match and not line.startswith('"file"'):
                    if current_var and current_occurrences:
                        result[current_var] = {"occurrences": current_occurrences}
                    current_var = var_match.group(1)
                    current_occurrences = []
                    continue
                
                # Parse occurrence entry
                if '"file"' in line:
                    try:
                        # Extract fields using regex
                        file_match = re.search(r'"file":"([^"]+)"', line)
                        func_match = re.search(r'"func":"([^"]+)"', line)
                        line_match = re.search(r'"line":(\d+)', line)
                        type_match = re.search(r'"type":"([^"]+)"', line)
                        
                        # For context, extract everything after "context":"
                        context_match = re.search(r'"context":"(.+?)"\s*[,}]', line)
                        context = context_match.group(1) if context_match else ""
                        
                        if file_match and func_match:
                            current_occurrences.append({
                                "file": file_match.group(1),
                                "func": func_match.group(1),
                                "line": int(line_match.group(1)) if line_match else 0,
                                "type": type_match.group(1) if type_match else "",
                                "context": context[:500],  # Truncate
                            })
                    except Exception:
                        pass
        
        # Don't forget the last variable
        if current_var and current_occurrences:
            result[current_var] = {"occurrences": current_occurrences}
        
        return result

    def close(self):
        self.cache.close()


def main():
    parser = argparse.ArgumentParser(
        description='Import analysis results into KV Cache memory',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
    # Import Linux kernel mm/ subsystem
    %(prog)s ./linux_subsystems/mm_cache /code/linux/mm
    
    # Import nginx
    %(prog)s ./nginx_cache /code/nginx
    
    # Use custom cache directory
    %(prog)s ./analysis_output /code/myproject --cache-dir ./my_memory
        """
    )
    
    parser.add_argument('analysis_dir', help='Directory containing analysis output')
    parser.add_argument('namespace', help='Target namespace (e.g., /code/linux/mm)')
    parser.add_argument('--cache-dir', '-d', default='./ai_code_memory',
                        help='KV Cache directory (default: ./ai_code_memory)')
    parser.add_argument('--max-memory', '-m', type=int, default=2048,
                        help='Max cache memory in MB (default: 2048)')
    
    args = parser.parse_args()
    
    importer = AnalysisImporter(
        cache_dir=args.cache_dir,
        max_memory=args.max_memory * 1024 * 1024
    )
    
    try:
        success = importer.import_analysis(args.analysis_dir, args.namespace)
        sys.exit(0 if success else 1)
    except KeyboardInterrupt:
        log("Interrupted by user")
        sys.exit(130)
    finally:
        importer.close()


if __name__ == '__main__':
    main()
