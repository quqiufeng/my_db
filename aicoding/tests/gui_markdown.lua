local function log(msg)
    print("[TEST] " .. tostring(msg))
end

-- Directly append an assistant message with markdown + diff code block.
local markdown_msg = [[
**README.md**
```text
# Mid Project
```

**Updated `lib.lua`** to include:
```lua
function greet(name)
    return 'Hello, ' .. name
end
```

**Verification (`grep 'Hello,' lib.lua`):**
```
    return 'Hello, ' .. name
```

**Git diff:**
```diff
diff --git a/lib.lua b/lib.lua
index 760c948..4780dda 100644
--- a/lib.lua
+++ b/lib.lua
@@ -1 +1,6 @@
 module Mid
+
+function greet(name)
+    return 'Hello, ' .. name
+end
+
```
]]

gui.append_message(app, session_id, "assistant", markdown_msg)

local function wait(deadline_seconds)
    local deadline = os.time() + (deadline_seconds or 10)
    while os.time() < deadline do
        coroutine.yield()
    end
end

log("waiting for GUI to render...")
wait(5)

local msgs = gui.get_messages(app)
local last = msgs[#msgs]
if not last or last.role ~= "assistant" then
    log("FAILED: last message not assistant")
    error("last message not assistant")
end

if not (last.text:find("README") and last.text:find("diff")) then
    log("FAILED: message text missing expected content")
    error("message text missing expected content")
end

log("rendered assistant markdown message")
log("PASSED")
os.exit(0)
