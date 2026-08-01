-- memory.lua project-scoped namespace tests
local t = require("testkit")
t.set_name("memory")
local memory = require("memory")

t.eq(memory.encode_project("/tmp/aicoding_demo"), "tmp_aicoding_demo", "encodes path")
t.eq(memory.encode_project("/"), nil, "root encodes to nil")
t.eq(memory.encode_project("."), nil, "dot encodes to nil")
t.eq(memory.encode_project("/a/b/c"), "a_b_c", "encodes nested path")
t.eq(memory.encode_project("/proj with space/x"), "proj_with_space_x", "sanitizes special chars")

memory.set_project("/tmp/aicoding_demo")
t.eq(memory.project_ns("facts"), "/project/tmp_aicoding_demo/facts", "facts ns is project-scoped")
t.eq(memory.project_ns("history"), "/project/tmp_aicoding_demo/history", "history ns is project-scoped")

-- Write and recall a fact under the project namespace.
memory.fact("unit_probe", "dir-shared")
t.eq(memory.recall("unit_probe"), "dir-shared", "fact round-trips in project ns")
local full = memory.key("unit_probe", memory.project_ns("facts"))
t.truthy(full:match("^/project/tmp_aicoding_demo/facts/"), "key lives under project ns")

-- Without a project, falls back to session namespace.
memory.set_project(nil)
t.eq(memory.project_ns("facts"), "/agent/" .. memory.session .. "/facts", "falls back to session ns")
