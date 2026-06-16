-- checkpoint.lua - 编辑前自动备份与回滚
--
-- 设计：
--   1. 每次破坏性编辑前调用 checkpoint.create(session_id, files)
--   2. 原文件复制到 .opencode/checkpoints/{session}/{timestamp}/
--   3. manifest 写入 KV Cache /agent/{session}/checkpoints/manifest
--   4. undo_last 恢复最近一次 checkpoint 的所有文件

local cjson = require("cjson")
local json = require("json")
local memory = require("memory")
local log = require("log")
local shell = require("shell")

local M = {}

-- 获取 checkpoint 根目录
local function checkpoint_root(session_id, project_root)
    project_root = project_root or "."
    return shell.runtime_dir(project_root, "checkpoints/" .. (session_id or "default"))
end

-- 把绝对路径转成相对路径（用于备份结构）
local function relative_path(project_root, abs_path)
    if abs_path:sub(1, #project_root) == project_root then
        local rel = abs_path:sub(#project_root + 1)
        if rel:sub(1, 1) == "/" then rel = rel:sub(2) end
        return rel
    end
    return abs_path
end

-- 复制文件，保留目录结构
local function copy_file(src, dst)
    local dir = dst:match("^(.*)/[^/]$")
    if dir then
        shell.mkdir_p(dir)
    end
    local in_f = io.open(src, "rb")
    if not in_f then return false, "cannot read " .. src end
    local data = in_f:read("*a")
    in_f:close()
    local out_f = io.open(dst, "wb")
    if not out_f then return false, "cannot write " .. dst end
    out_f:write(data)
    out_f:close()
    return true, nil
end

-- 创建 checkpoint
-- files: 数组 of {path=absolute_path, reason="edit|write|delete"}
function M.create(session_id, project_root, files, reason)
    if not files or #files == 0 then
        return { ok = true, note = "no files to checkpoint" }
    end

    local ts = tostring(os.time())
    local root = checkpoint_root(session_id, project_root)
    local cp_dir = root .. "/" .. ts
    shell.mkdir_p(cp_dir)

    local backed = {}
    for _, f in ipairs(files) do
        local src = f.path
        local rel = relative_path(project_root, src)
        local dst = cp_dir .. "/" .. rel
        -- Only backup if file exists (for delete, the file exists before deletion)
        local attr = io.open(src, "rb")
        if attr then
            attr:close()
            local ok, err = copy_file(src, dst)
            if ok then
                table.insert(backed, rel)
            end
        end
    end

    -- Read existing manifest
    local manifest_key = "/agent/" .. session_id .. "/checkpoints/manifest"
    local manifest_json = memory.read(manifest_key)
    local manifest = {}
    if manifest_json then
        local v, _ = json.decode(manifest_json)
        if v and type(v) == "table" then manifest = v end
    end

    table.insert(manifest, 1, {
        id = ts,
        reason = reason or "edit",
        time = os.date("%Y-%m-%d %H:%M:%S"),
        files = backed,
        dir = cp_dir
    })

    -- Keep only last 20 checkpoints
    while #manifest > 20 do table.remove(manifest) end

    memory.write(manifest_key, cjson.encode(manifest))
    log.info("checkpoint created: %s (%d files)", ts, #backed)

    return { ok = true, checkpoint_id = ts, files_backed = backed, dir = cp_dir }
end

-- 列出 checkpoints
function M.list(session_id)
    local manifest_json = memory.read("/agent/" .. session_id .. "/checkpoints/manifest")
    if not manifest_json then
        return { ok = true, checkpoints = {} }
    end
    local manifest, _ = json.decode(manifest_json)
    if not manifest then
        return { ok = false, error = "manifest parse error" }
    end
    return { ok = true, checkpoints = manifest }
end

-- 恢复一个 checkpoint
function M.restore(session_id, project_root, checkpoint_id)
    local list_result = M.list(session_id)
    if not list_result.ok then return list_result end

    local cp = nil
    for _, c in ipairs(list_result.checkpoints) do
        if c.id == checkpoint_id then
            cp = c
            break
        end
    end
    if not cp then
        return { ok = false, error = "checkpoint not found: " .. tostring(checkpoint_id) }
    end

    local restored = {}
    for _, rel in ipairs(cp.files) do
        local src = cp.dir .. "/" .. rel
        local dst = project_root .. "/" .. rel
        local ok, err = copy_file(src, dst)
        if ok then
            table.insert(restored, rel)
        else
            log.error("restore failed for %s: %s", rel, err)
            return { ok = false, error = err, restored = restored }
        end
    end

    return { ok = true, checkpoint_id = checkpoint_id, restored = restored }
end

-- 撤销最近一次 checkpoint
function M.undo_last(session_id, project_root)
    log.info("undo_last requested for session %s", session_id)
    local list_result = M.list(session_id)
    if not list_result.ok then return list_result end
    if #list_result.checkpoints == 0 then
        return { ok = false, error = "no checkpoints to undo" }
    end
    local cp = list_result.checkpoints[1]
    return M.restore(session_id, project_root, cp.id)
end

return M
