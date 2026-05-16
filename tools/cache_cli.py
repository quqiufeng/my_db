#!/usr/bin/env python3
"""
Cache CLI — 命令行接口 for KV Cache

用法:
    # 基础 CRUD
    cache set <key> <value> [--ttl <ms>] [--cache-dir <dir>]
    cache get <key> [--cache-dir <dir>]
    cache del <key> [--cache-dir <dir>]
    cache exists <key> [--cache-dir <dir>]
    
    # 列表和搜索
    cache list [--prefix <prefix>] [--limit <n>] [--cache-dir <dir>]
    cache search --prefix <query> [--limit <n>]
    cache search --regex <pattern> [--limit <n>]
    cache search --fuzzy <query> [--limit <n>]
    cache search --tag <keyword> [--limit <n>]
    
    # 管理和统计
    cache stats [--cache-dir <dir>]
    cache compact [--cache-dir <dir>]
    cache purge [--cache-dir <dir>]
    cache check <dir>
    
    # 导入
    cache import-book <file> [namespace] [--cache-dir <dir>]
    cache import-github <url> [namespace] [--cache-dir <dir>]

环境变量:
    MYDB_CACHE_DIR — 默认 cache 目录 (默认: ./cache_data)
"""

import os
import sys
import argparse
import json

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from mydb.cache import open_cache, check

DEFAULT_CACHE_DIR = os.environ.get('MYDB_CACHE_DIR', './cache_data')


def get_cache(args):
    """获取 cache 实例"""
    cache_dir = getattr(args, 'cache_dir', DEFAULT_CACHE_DIR)
    return open_cache(cache_dir, 100 * 1024 * 1024)


def cmd_set(args):
    with get_cache(args) as cache:
        cache.set(args.key, args.value, getattr(args, 'ttl', 0))
        print(f"OK: {args.key}")


def cmd_get(args):
    with get_cache(args) as cache:
        value = cache.get(args.key)
        if value is None:
            print(f"(not found)", file=sys.stderr)
            sys.exit(1)
        print(value)


def cmd_del(args):
    with get_cache(args) as cache:
        try:
            cache.delete(args.key)
            print(f"OK: deleted {args.key}")
        except KeyError:
            print(f"(not found)", file=sys.stderr)
            sys.exit(1)


def cmd_exists(args):
    with get_cache(args) as cache:
        print("yes" if args.key in cache else "no")


def cmd_list(args):
    with get_cache(args) as cache:
        prefix = getattr(args, 'prefix', '')
        limit = getattr(args, 'limit', 100)
        count = 0
        
        for k, v in cache.items():
            if prefix and not k.startswith(prefix):
                continue
            print(f"{k}")
            count += 1
            if count >= limit:
                print(f"... ({cache.count} total)")
                break
        
        if count == 0:
            print("(empty)")
        else:
            print(f"\nTotal: {cache.count} entries")


def cmd_search(args):
    with get_cache(args) as cache:
        limit = getattr(args, 'limit', 100)
        results = []
        
        if args.prefix:
            results = cache.search_prefix(args.prefix, max_results=limit)
        elif args.regex:
            results = cache.search_regex(args.regex, max_results=limit)
        elif args.fuzzy:
            results = cache.search_fuzzy(args.fuzzy, max_results=limit)
        elif args.tag:
            results = cache.search_tag(args.tag, max_results=limit)
        else:
            print("Error: 请指定 --prefix/--regex/--fuzzy/--tag", file=sys.stderr)
            sys.exit(1)
        
        if not results:
            print("(no results)")
            return
        
        for r in results:
            score_str = f" [{r['score']:.2f}]" if r['score'] < 1.0 else ""
            value_preview = r['value'][:80].replace('\n', ' ')
            print(f"  {r['key']}{score_str}")
            print(f"    {value_preview}...")
        
        print(f"\n{len(results)} result(s)")


def cmd_stats(args):
    with get_cache(args) as cache:
        print(f"Cache: {args.cache_dir}")
        print(f"  Entries: {cache.count}")
        print(f"  Memory:  {cache.memory_used / 1024:.1f} KB / {cache.memory_max / 1024 / 1024:.0f} MB")
        print(f"  Usage:   {cache.memory_used / cache.memory_max * 100:.1f}%")


def cmd_compact(args):
    with get_cache(args) as cache:
        count = cache.compact()
        print(f"Compacted: {count} entries reclaimable")


def cmd_purge(args):
    with get_cache(args) as cache:
        count = cache.purge_expired()
        print(f"Purged: {count} expired entries")


def cmd_check(args):
    if check(args.dir):
        print(f"OK: {args.dir} is valid")
    else:
        print(f"CORRUPTED: {args.dir}", file=sys.stderr)
        sys.exit(1)


def cmd_import_book(args):
    import subprocess
    cmd = [
        os.path.join(os.path.dirname(__file__), 'import_book'),
        args.cache_dir,
        args.file,
    ]
    if args.namespace:
        cmd.append(args.namespace)
    subprocess.run(cmd)


def cmd_import_github(args):
    import subprocess
    cmd = [
        sys.executable,
        os.path.join(os.path.dirname(__file__), 'import_github.py'),
        args.url,
        args.cache_dir,
    ]
    if args.namespace:
        cmd.extend(['--namespace', args.namespace])
    subprocess.run(cmd)


def main():
    parser = argparse.ArgumentParser(
        description='KV Cache CLI',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
    cache set /coding/cpp/move "右值引用实现完美转发"
    cache get /coding/cpp/move
    cache list --prefix /coding/cpp
    cache search --prefix /coding
    cache search --regex ".*async.*"
    cache stats
    cache import-book ~/book.mobi /books/cpp
    cache import-github https://github.com/redis/redis
        """
    )
    
    parser.add_argument('--cache-dir', '-d', default=DEFAULT_CACHE_DIR,
                        help=f'Cache 目录 (默认: {DEFAULT_CACHE_DIR})')
    
    subparsers = parser.add_subparsers(dest='command', help='命令')
    
    # set
    p_set = subparsers.add_parser('set', help='设置 key-value')
    p_set.add_argument('key', help='key')
    p_set.add_argument('value', help='value')
    p_set.add_argument('--ttl', type=int, default=0, help='TTL (毫秒, 0=永久)')
    p_set.set_defaults(func=cmd_set)
    
    # get
    p_get = subparsers.add_parser('get', help='获取 value')
    p_get.add_argument('key', help='key')
    p_get.set_defaults(func=cmd_get)
    
    # del
    p_del = subparsers.add_parser('del', help='删除 key')
    p_del.add_argument('key', help='key')
    p_del.set_defaults(func=cmd_del)
    
    # exists
    p_exists = subparsers.add_parser('exists', help='检查 key 是否存在')
    p_exists.add_argument('key', help='key')
    p_exists.set_defaults(func=cmd_exists)
    
    # list
    p_list = subparsers.add_parser('list', help='列出所有 key')
    p_list.add_argument('--prefix', '-p', help='前缀过滤')
    p_list.add_argument('--limit', '-n', type=int, default=100, help='最大数量')
    p_list.set_defaults(func=cmd_list)
    
    # search
    p_search = subparsers.add_parser('search', help='搜索')
    p_search.add_argument('--prefix', help='前缀搜索')
    p_search.add_argument('--regex', help='正则搜索')
    p_search.add_argument('--fuzzy', help='模糊搜索')
    p_search.add_argument('--tag', help='标签搜索')
    p_search.add_argument('--limit', '-n', type=int, default=100, help='最大数量')
    p_search.set_defaults(func=cmd_search)
    
    # stats
    p_stats = subparsers.add_parser('stats', help='统计信息')
    p_stats.set_defaults(func=cmd_stats)
    
    # compact
    p_compact = subparsers.add_parser('compact', help='物理清理')
    p_compact.set_defaults(func=cmd_compact)
    
    # purge
    p_purge = subparsers.add_parser('purge', help='清理过期条目')
    p_purge.set_defaults(func=cmd_purge)
    
    # check
    p_check = subparsers.add_parser('check', help='检查 cache 完整性')
    p_check.add_argument('dir', help='cache 目录')
    p_check.set_defaults(func=cmd_check)
    
    # import-book
    p_ibook = subparsers.add_parser('import-book', help='导入电子书')
    p_ibook.add_argument('file', help='电子书文件路径')
    p_ibook.add_argument('namespace', nargs='?', help='namespace')
    p_ibook.set_defaults(func=cmd_import_book)
    
    # import-github
    p_igh = subparsers.add_parser('import-github', help='导入 GitHub 仓库')
    p_igh.add_argument('url', help='GitHub URL')
    p_igh.add_argument('namespace', nargs='?', help='namespace')
    p_igh.set_defaults(func=cmd_import_github)
    
    args = parser.parse_args()
    
    if not args.command:
        parser.print_help()
        sys.exit(1)
    
    args.func(args)


if __name__ == '__main__':
    main()
