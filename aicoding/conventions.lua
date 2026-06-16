-- conventions.lua - project-type aware best-practice injection
--
-- Goal: keep the system prompt generic while giving the model concrete,
-- project-specific workflow guidance.  We detect the project type from the
-- file tree, then load a matching convention file.  Users can override with
-- .opencode/conventions.md or .opencode/conventions/{type}.md.
--
-- This is NOT about adding new tools.  It is about telling the model HOW to
-- use the existing tools for this kind of project.

local shell = require("shell")

local M = {}

-- Project type markers.  A type is recognised when ANY marker exists.
-- Order matters: earlier types are checked first.
M.PROJECT_TYPES = {
    {
        name = "linux_kernel",
        markers = {
            "init/main.c",
            "kernel/panic.c",
            "scripts/checkpatch.pl",
            "include/linux",
            "Kconfig",
        },
    },
    {
        name = "cargo",
        markers = { "Cargo.toml" },
    },
    {
        name = "npm",
        markers = { "package.json" },
    },
    {
        name = "python",
        markers = { "pyproject.toml", "setup.py", "requirements.txt" },
    },
    {
        name = "go",
        markers = { "go.mod" },
    },
    {
        name = "cmake",
        markers = { "CMakeLists.txt" },
    },
}

-- Built-in convention directory (same dir as this file).
local function builtin_dir()
    local path = debug.getinfo(1, "S").source:sub(2)
    -- path may be "@/opt/my_db/aicoding/conventions.lua"
    return path:match("^(.*)/[^/]+$") or "."
end

local function file_exists(path)
    local f = io.open(path, "r")
    if f then
        f:close()
        return true
    end
    return false
end

-- Detect project type from marker files.
function M.detect_type(project_root)
    project_root = project_root or "."
    for _, ptype in ipairs(M.PROJECT_TYPES) do
        for _, marker in ipairs(ptype.markers) do
            if file_exists(project_root .. "/" .. marker) then
                return ptype.name
            end
        end
    end
    return "generic"
end

-- Load a convention file if it exists.
local function load_file(path)
    local f = io.open(path, "r")
    if not f then return nil end
    local content = f:read("*a")
    f:close()
    if content and content:match("%S") then
        return content
    end
    return nil
end

-- Load conventions for a project.
-- Priority:
--   1. .opencode/conventions.md
--   2. .opencode/conventions/{type}.md
--   3. aicoding/conventions/{type}.md
--   4. aicoding/conventions/generic.md
function M.load(project_root)
    project_root = project_root or "."
    local ptype = M.detect_type(project_root)
    local base = builtin_dir()

    local candidates = {
        project_root .. "/.opencode/conventions.md",
        project_root .. "/.opencode/conventions/" .. ptype .. ".md",
        base .. "/conventions/" .. ptype .. ".md",
        base .. "/conventions/generic.md",
    }

    for _, path in ipairs(candidates) do
        local content = load_file(path)
        if content then
            return content, ptype, path
        end
    end

    return nil, ptype, nil
end

-- Extract just the workflow section (first ## Workflow ... section).
function M.workflow(project_root)
    local content, ptype, path = M.load(project_root)
    if not content then return nil, ptype end

    -- Find first "## Workflow" section.
    local start = content:find("\n## Workflow")
    if not start then
        start = content:find("^## Workflow")
    end
    if not start then return content, ptype end

    local next_section = content:find("\n## ", start + 1)
    local wf
    if next_section then
        wf = content:sub(start + 1, next_section)
    else
        wf = content:sub(start + 1)
    end
    return wf, ptype
end

-- Extract checklist / common mistakes / useful commands if present.
function M.sections(project_root)
    local content, ptype, path = M.load(project_root)
    if not content then return {}, ptype end

    local sections = {}
    local pattern = "\n## ([^\n]+)\n"
    local pos = 1
    while true do
        local h_start, h_end, title = content:find(pattern, pos)
        if not h_start then break end
        local body_start = h_end + 1
        local next_h = content:find("\n## ", body_start)
        local body_end = next_h and (next_h - 1) or #content
        sections[title] = content:sub(body_start, body_end):gsub("^%s+", ""):gsub("%s+$", "")
        pos = body_end + 1
    end

    return sections, ptype
end

return M
