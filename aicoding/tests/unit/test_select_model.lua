-- select_model / model resolution tests (public behavior only;
-- load_models_file is module-private)
local t = require("testkit")
t.set_name("select_model")

local first = select_model("")
t.truthy(first, "select_model('') returns a name")

-- Unknown --model name falls back to the first configured model.
local fb = select_model("definitely-not-a-model-xyz")
t.truthy(fb, "unknown model falls back")
t.eq(fb, first, "fallback equals default model")

-- select_model(nil) behaves like select_model("").
t.eq(select_model(nil), first, "nil behaves like empty")

-- With an explicit name that exists, it resolves to that model.
local named = select_model(first)
t.eq(named, first, "explicit name resolves")

-- Side effect: selecting a model sets OPENAI_MODEL / LLM_PROTOCOL env.
local proto = os.getenv("LLM_PROTOCOL")
t.truthy(proto == "openai" or proto == "anthropic", "protocol env set")
t.ok(true, "done")
