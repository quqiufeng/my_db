#!/usr/bin/env luajit
-- book_reader.lua - 通过 FFI 读取 KV Cache 中的电子书
-- 用法: luajit book_reader.lua [cache_dir]
-- 支持: 列出书籍、按章节阅读、全文搜索

local ffi = require("ffi")

-- 定义 C 接口
ffi.cdef[[
    typedef struct cache cache_t;
    typedef struct cache_iter cache_iter_t;
    
    cache_t* cache_open(const char* db_dir, size_t max_memory);
    void cache_close(cache_t* cache);
    const char* cache_get(cache_t* cache, const char* key);
    size_t cache_count(cache_t* cache);
    
    cache_iter_t* cache_iter_create(cache_t* cache);
    void cache_iter_destroy(cache_iter_t* iter);
    int cache_iter_next(cache_iter_t* iter, const char** key_out, const char** value_out);
]]

-- 加载库
local lib_paths = {
    "./libmydb.so",
    "../libmydb.so",
    "libmydb.so",
    "/usr/local/lib/libmydb.so",
}

local lib = nil
for _, path in ipairs(lib_paths) do
    local ok, handle = pcall(ffi.load, path)
    if ok then
        lib = handle
        print("✓ Loaded: " .. path)
        break
    end
end

if not lib then
    print("✗ Error: Cannot load libmydb.so")
    os.exit(1)
end

-- 打开 cache
local cache_dir = arg[1] or "./test_books_cache"
local cache = lib.cache_open(cache_dir, 100 * 1024 * 1024)
if cache == nil then
    print("✗ Error: Cannot open cache: " .. cache_dir)
    os.exit(1)
end

print("✓ Cache opened: " .. cache_dir)
print("  Entries: " .. tonumber(lib.cache_count(cache)))
print("")

-- 获取值（用于非迭代场景）
local function cache_get(key)
    local ptr = lib.cache_get(cache, key)
    if ptr == nil then return nil end
    return ffi.string(ptr)
end

-- 简单 JSON 内容提取
local function extract_content(json_str)
    if not json_str then return "" end
    -- 提取 "c":"..." 的内容（支持转义字符）
    local content = json_str:match('"c":"(.-)"')
    if content then
        -- 处理转义字符
        content = content:gsub('\\n', '\n'):gsub('\\t', '\t')
        return content
    end
    return json_str
end

-- 列出所有书
local function list_books()
    local books = {}
    local iter = lib.cache_iter_create(cache)
    local key_ptr = ffi.new("const char*[1]")
    local val_ptr = ffi.new("const char*[1]")
    
    while lib.cache_iter_next(iter, key_ptr, val_ptr) == 1 do
        local key = ffi.string(key_ptr[0])
        -- 使用字符串操作而非正则：匹配 /books/{ns}/_meta/title
        if key:sub(1, 7) == "/books/" and key:sub(-12) == "/_meta/title" then
            local book_ns = key:sub(8, -13)  -- 提取 namespace
            local title_data = cache_get(key)
            local title = book_ns
            if title_data then
                title = extract_content(title_data)
                if title == "" then title = book_ns end
            end
            table.insert(books, {
                ns = book_ns,
                title = title
            })
        end
    end
    
    lib.cache_iter_destroy(iter)
    return books
end

-- 获取书的章节列表
local function get_chapters(book_ns)
    local chapters = {}
    local iter = lib.cache_iter_create(cache)
    local key_ptr = ffi.new("const char*[1]")
    local val_ptr = ffi.new("const char*[1]")
    local prefix = "/books/" .. book_ns .. "/chapters/"
    
    while lib.cache_iter_next(iter, key_ptr, val_ptr) == 1 do
        local key = ffi.string(key_ptr[0])
        -- 快速前缀 + 后缀检查
        if key:sub(1, #prefix) == prefix and key:sub(-6) == "/title" then
            -- 提取 chapter_id: /books/{ns}/chapters/{id}/title
            local start_pos = #prefix + 1
            local end_pos = key:find("/title", start_pos, true)
            if end_pos then
                local chapter_id = key:sub(start_pos, end_pos - 1)
                local title_data = ffi.string(val_ptr[0])
                table.insert(chapters, {
                    id = chapter_id,
                    title = extract_content(title_data) or chapter_id
                })
            end
        end
    end
    
    lib.cache_iter_destroy(iter)
    
    -- 按章节 ID 排序
    table.sort(chapters, function(a, b)
        return a.id < b.id
    end)
    
    return chapters
end

-- 获取章节内容
local function get_chapter_content(book_ns, chapter_id)
    local paragraphs = {}
    local iter = lib.cache_iter_create(cache)
    local key_ptr = ffi.new("const char*[1]")
    local val_ptr = ffi.new("const char*[1]")
    local prefix = "/books/" .. book_ns .. "/chapters/" .. chapter_id .. "/content/"
    
    while lib.cache_iter_next(iter, key_ptr, val_ptr) == 1 do
        local key = ffi.string(key_ptr[0])
        if key:sub(1, #prefix) == prefix then
            local para_id = key:match("p%d+$")
            if para_id then
                local content = ffi.string(val_ptr[0])
                if content then
                    table.insert(paragraphs, {
                        id = para_id,
                        content = extract_content(content)
                    })
                end
            end
        end
    end
    
    lib.cache_iter_destroy(iter)
    
    -- 按段落 ID 排序
    table.sort(paragraphs, function(a, b)
        return a.id < b.id
    end)
    
    return paragraphs
end

-- 搜索功能
local function search_books(keyword)
    local results = {}
    local iter = lib.cache_iter_create(cache)
    local key_ptr = ffi.new("const char*[1]")
    local val_ptr = ffi.new("const char*[1]")
    local keyword_lower = keyword:lower()
    local count = 0
    
    print("🔍 Searching for: '" .. keyword .. "' ...")
    
    while lib.cache_iter_next(iter, key_ptr, val_ptr) == 1 do
        local key = ffi.string(key_ptr[0])
        -- 只搜索内容段落: /books/{ns}/chapters/{id}/content/pNNNN
        if key:sub(1, 7) == "/books/" and key:find("/content/p", 1, true) then
            local value = ffi.string(val_ptr[0])
            if value:lower():find(keyword_lower, 1, true) then
                local book_ns = key:match("^/books/([^/]+)/")
                local chapter_id = key:match("/chapters/([^/]+)/")
                table.insert(results, {
                    book = book_ns,
                    chapter = chapter_id,
                    preview = value:sub(1, 120)
                })
                count = count + 1
                if count >= 50 then
                    print("  (Found 50+ matches, stopping...)")
                    break
                end
            end
        end
    end
    
    lib.cache_iter_destroy(iter)
    return results
end

-- 显示书的信息
local function show_book_info(book_ns)
    local title = cache_get("/books/" .. book_ns .. "/_meta/title")
    local author = cache_get("/books/" .. book_ns .. "/_meta/author")
    
    print(string.rep("=", 60))
    print("📖 Book: " .. (extract_content(title) or book_ns))
    print("✍️  Author: " .. (author or "Unknown"))
    print(string.rep("=", 60))
end

-- 清屏
local function clear_screen()
    os.execute("clear")
end

-- 自动阅读：分页显示并自动翻页
local function auto_read_book(book_ns)
    local chapters = get_chapters(book_ns)
    if #chapters == 0 then
        print("No chapters found")
        return
    end
    
    local title = cache_get("/books/" .. book_ns .. "/_meta/title")
    local book_title = extract_content(title) or book_ns
    
    print("\n📖 Auto reading: " .. book_title)
    print("   " .. #chapters .. " chapters, 3 seconds per page")
    print("   Press Ctrl+C to stop\n")
    print("Starting in 3 seconds...")
    os.execute("sleep 3")
    
    local total_pages = 0
    local lines_per_page = 30
    
    for ch_idx, chapter in ipairs(chapters) do
        local paragraphs = get_chapter_content(book_ns, chapter.id)
        
        if #paragraphs > 0 then
            -- 分页
            local pages = {}
            local current_page = {}
            local current_lines = 0
            
            for _, para in ipairs(paragraphs) do
                local lines = 1
                for _ in para.content:gmatch("\n") do
                    lines = lines + 1
                end
                
                if current_lines + lines > lines_per_page and #current_page > 0 then
                    table.insert(pages, current_page)
                    current_page = {para}
                    current_lines = lines
                else
                    table.insert(current_page, para)
                    current_lines = current_lines + lines
                end
            end
            
            if #current_page > 0 then
                table.insert(pages, current_page)
            end
            
            -- 显示
            for page_idx, page_paras in ipairs(pages) do
                total_pages = total_pages + 1
                clear_screen()
                
                print(string.rep("═", 70))
                print("📖 " .. book_title .. " - " .. chapter.title)
                print(string.rep("═", 70))
                print("")
                
                for _, para in ipairs(page_paras) do
                    print(para.content)
                    print("")
                end
                
                print(string.rep("─", 70))
                print(string.format("Page: %d / %d  (Chapter %d / %d)", 
                    page_idx, #pages, ch_idx, #chapters))
                print(string.rep("═", 70))
                
                os.execute("sleep 3")
            end
        end
    end
    
    clear_screen()
    print(string.rep("═", 70))
    print("✓ Reading complete!")
    print("📖 " .. book_title)
    print("   " .. #chapters .. " chapters, " .. total_pages .. " pages")
    print(string.rep("═", 70))
end

-- 主菜单
local function main_menu()
    while true do
        print("\n" .. string.rep("─", 60))
        print("📚 E-Book Reader Menu")
        print(string.rep("─", 60))
        print("1. List all books")
        print("2. Read a book (manual)")
        print("3. Search in all books")
        print("4. Auto read a book")
        print("5. Exit")
        io.write("\nChoice: ")
        local choice = io.read("*l")
        if not choice then break end
        choice = choice:gsub("^%s*(.-)%s*$", "%1")
        
        if choice == "1" then
            local books = list_books()
            print("\n📖 Available Books:")
            for i, book in ipairs(books) do
                print(string.format("  [%d] %s", i, book.title))
                print(string.format("      (namespace: %s)", book.ns))
            end
            
        elseif choice == "2" then
            local books = list_books()
            print("\nSelect a book:")
            for i, book in ipairs(books) do
                print(string.format("  [%d] %s", i, book.title))
            end
            io.write("\nBook number: ")
            local num_str = io.read("*l")
            if not num_str then break end
            local num_str_trimmed = num_str:gsub("^%s*(.-)%s*$", "%1")
            local num = tonumber(num_str_trimmed)
            
            if num and num >= 1 and num <= #books then
                local book = books[num]
                show_book_info(book.ns)
                
                local chapters = get_chapters(book.ns)
                print("\n📑 Chapters (" .. #chapters .. " total):")
                local display_count = math.min(20, #chapters)
                for i = 1, display_count do
                    print(string.format("  [%d] %s", i, chapters[i].title))
                end
                if #chapters > 20 then
                    print("  ... and " .. (#chapters - 20) .. " more chapters")
                end
                
                io.write("\nChapter number (or 'back'): ")
                local ch_num = io.read("*l")
                if not ch_num then break end
                ch_num = ch_num:gsub("^%s*(.-)%s*$", "%1")
                
                if ch_num ~= "back" then
                    local ch_idx = tonumber(ch_num)
                    if ch_idx and ch_idx >= 1 and ch_idx <= #chapters then
                        local chapter = chapters[ch_idx]
                        print("\n" .. string.rep("═", 60))
                        print("📖 " .. chapter.title)
                        print(string.rep("═", 60))
                        
                        local paragraphs = get_chapter_content(book.ns, chapter.id)
                        print("\n📝 Content (" .. #paragraphs .. " paragraphs):\n")
                        
                        for _, para in ipairs(paragraphs) do
                            print(para.content)
                            print("")
                        end
                        
                        io.write("\nPress Enter to continue...")
                        io.read("*l")
                    else
                        print("Invalid chapter number")
                    end
                end
            else
                print("Invalid book number")
            end
            
        elseif choice == "3" then
            io.write("Search keyword: ")
            local keyword = io.read("*l")
            if not keyword then break end
            keyword = keyword:gsub("^%s*(.-)%s*$", "%1")
            
            if #keyword > 0 then
                local results = search_books(keyword)
                print("\n🔍 Found " .. #results .. " matches:")
                for i, result in ipairs(results) do
                    print(string.format("\n[%d] %s / %s", i, result.book, result.chapter))
                    print("    " .. result.preview:gsub("\n", " "))
                end
            else
                print("Empty keyword")
            end
            
        elseif choice == "4" then
            local books = list_books()
            print("\nSelect a book to auto read:")
            for i, book in ipairs(books) do
                print(string.format("  [%d] %s", i, book.title))
            end
            io.write("\nBook number: ")
            local num_str = io.read("*l")
            if not num_str then break end
            local num_str_trimmed = num_str:gsub("^%s*(.-)%s*$", "%1")
            local num = tonumber(num_str_trimmed)
            
            if num and num >= 1 and num <= #books then
                auto_read_book(books[num].ns)
            else
                print("Invalid book number")
            end
            
        elseif choice == "5" or choice == "q" or choice == "quit" then
            break
        else
            print("Invalid choice, please try again")
        end
    end
end

-- 运行
print(string.rep("═", 60))
print("📚 E-Book Reader for KV Cache")
print(string.rep("═", 60))
print("Cache: " .. cache_dir)
print("")

main_menu()

lib.cache_close(cache)
print("\n✓ Goodbye!")
