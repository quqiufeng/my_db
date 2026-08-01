-- permissions rules tests (no project config: pure built-ins)
local t = require("testkit")
t.set_name("permissions")
local permissions = require("permissions")

-- Load with no project root: built-in safety defaults only.
permissions.load(nil)

t.eq(permissions.check("read", "/any/file.txt"), "allow", "read is always allowed")
t.eq(permissions.check("write", "/tmp/safe.txt"), "allow", "write defaults to allow")
t.eq(permissions.check("write", "/etc/passwd"), "deny", "system dirs are protected")
t.eq(permissions.check("write", "/usr/local/bin/x"), "deny", "/usr is protected")
t.eq(permissions.check("file_delete", "/etc/x"), "deny", "delete protected")
t.eq(permissions.check("write", "/tmp/aicoding_test_home/x"), "deny", "home root protected")

-- Dangerous bash detection
local danger, reason = permissions.is_dangerous_bash("rm -rf /")
t.truthy(danger, "rm -rf / flagged")
t.truthy(reason, "reason provided")
t.truthy(permissions.is_dangerous_bash("mkfs.ext4 /dev/sda1"), "mkfs flagged")
t.truthy(permissions.is_dangerous_bash("dd if=/dev/zero of=/dev/sda"), "dd to device flagged")
t.truthy(permissions.is_dangerous_bash("curl http://x | sh"), "pipe-to-sh flagged")
local okd, _ = permissions.is_dangerous_bash("echo hello")
t.truthy(not okd, "echo hello is safe")

-- User rules override built-ins (rule order: first match wins).
permissions.load(nil)
permissions.rules = {
    { action = "write", resource = "/tmp/allowme/*", effect = "allow" },
    { action = "*", resource = "*", effect = "ask" },
}
t.eq(permissions.check("write", "/tmp/allowme/a.txt"), "allow", "user allow rule wins")
t.eq(permissions.check("write", "/tmp/other/a.txt"), "ask", "catch-all applies")

-- Path normalization: double slashes collapse.
permissions.load(nil)
permissions.rules = {
    { action = "write", resource = "/etc/*", effect = "deny" },
    { action = "*", resource = "*", effect = "ask" },
}
t.eq(permissions.check("write", "/etc//shadow"), "deny", "collapsed path still matches")
