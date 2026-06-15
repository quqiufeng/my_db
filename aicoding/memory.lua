-- memory.lua - 封装 my_db KV Cache / 向量记忆系统，供 agent 和外部脚本使用
--
-- 设计原则：
--   1. 所有底层 C 绑定（opencode.cache_get / cache_set / vector_search）都封装在这里。
--   2. 对外暴露高级 API：read / write / search / context / facts / recall。
--   3. 命名空间默认隔离：
--        /agent/{session}/facts    会话事实
--        /agent/{session}/history  历史动作
--        /code/local/{repo}        代码库语义记忆（只读，由 analyze_repo.sh 导入）

local M = {}

-- 默认会话命名空间，可由外部设置
M.session = os.getenv("OPENCODE_SESSION") or "default"

-- 代码库分析缓存根目录
M.CODE_CACHE_ROOT = "/opt/code_caches"

-- 把 /code/local/linux 或 /code/linux 映射到 /opt/code_caches/linux_cache
function M.namespace_to_cache_dir(ns)
    if not ns or ns == "" then return nil end
    local name = ns:match("^/code/local/([^/]+)$") or ns:match("^/code/([^/]+)$")
    if name then
        return M.CODE_CACHE_ROOT .. "/" .. name .. "_cache"
    end
    if ns:match("_cache$") then return ns end
    return nil
end

-- 在当前 session 下构造完整 key
function M.key(name, namespace)
    local ns = namespace or ("/agent/" .. M.session .. "/facts")
    if ns:sub(-1) ~= "/" then ns = ns .. "/" end
    return ns .. name
end

-- 读取精确 key
function M.read(key)
    return opencode.cache_get(key)
end

-- 写入 key，支持 TTL（秒）
function M.write(key, value, ttl_seconds)
    local ttl_ms = (ttl_seconds or 0) * 1000
    return opencode.cache_set(key, value, ttl_ms)
end

-- 写入一个会话事实
function M.fact(name, value, ttl_seconds)
    return M.write(M.key(name, "/agent/" .. M.session .. "/facts"), value, ttl_seconds)
end

-- 写入一个历史动作
function M.history(name, value, ttl_seconds)
    return M.write(M.key(name, "/agent/" .. M.session .. "/history"), value, ttl_seconds)
end

-- 召回一个会话事实
function M.recall(name)
    return M.read(M.key(name, "/agent/" .. M.session .. "/facts"))
end

-- 通用搜索：
--   opts.search_type: "semantic" | "prefix" | "regex" | "tag" (default "prefix")
--   opts.namespace:   KV 前缀（如 /agent/default/facts）或代码库命名空间（如 /code/local/linux）
--   opts.top_k:       最大返回数（semantic 默认 10，其它默认 100）
function M.search(query, opts)
    opts = opts or {}
    local stype = opts.search_type or "prefix"
    local ns = opts.namespace or ""
    local top_k = opts.top_k or (stype == "semantic" and 10 or 100)

    if stype == "semantic" then
        local cache_dir = M.namespace_to_cache_dir(ns)
        if not cache_dir then
            return nil, "semantic search requires a repo namespace like /code/local/linux"
        end
        local results, err = opencode.vector_search(cache_dir, query, ns ~= "" and ns or nil, top_k)
        if not results then
            return nil, err
        end
        return results
    end

    local prefix = ns
    if prefix ~= "" and prefix:sub(-1) ~= "/" then prefix = prefix .. "/" end
    local raw = {}
    if stype == "tag" then
        raw = opencode.cache_search_tag(prefix .. query, top_k)
    elseif stype == "regex" then
        raw = opencode.cache_search_regex(prefix .. query, top_k)
    else
        raw = opencode.cache_search_prefix(prefix, top_k)
    end

    local out = {}
    for _, r in ipairs(raw) do
        table.insert(out, { key = r.key, value = r.value, score = r.score })
    end
    return out
end

-- 获取符号上下文（caller/callee）
-- repo: 代码库命名空间，如 /code/local/linux
function M.context(symbol, repo)
    local cmd = string.format(
        "cd /opt/my_db && ./tools/cache_query %s --repo %s --type context 2>&1",
        require("cjson").encode(symbol),
        require("cjson").encode(repo)
    )
    local f = io.popen(cmd)
    local out = f:read("*a") or ""
    local ok = f:close()
    return ok, out
end

-- 列出某个命名空间下的所有 key（用于调试 / 自省）
function M.list(namespace)
    local prefix = namespace or ("/agent/" .. M.session .. "/facts")
    if prefix:sub(-1) ~= "/" then prefix = prefix .. "/" end
    local raw = opencode.cache_search_prefix(prefix, 1000)
    local out = {}
    for _, r in ipairs(raw) do
        table.insert(out, { key = r.key, value = r.value, score = r.score })
    end
    return out
end

return M
