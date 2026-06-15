local M = {}

function M.on_load()
    return {
        {
            name = "hello",
            description = "Greet someone by name",
            parameters = {
                name = { type = "string", description = "Name to greet" }
            },
            handler = function(args)
                return "Hello, " .. args.name .. "!"
            end
        }
    }
end

return M
