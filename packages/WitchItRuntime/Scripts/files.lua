return function()
    local F={}
    local windows=package.config:sub(1,1)=='\\'
    local function shell(s) return "'"..s:gsub("'","'\\''").."'" end
    -- Wine's cmd makes no parent directories for a path with forward slashes.
    local function native(s) return (s:gsub('/','\\')) end
    function F.read(path)
        local f,why,code=io.open(path,'rb')
        if not f then if code==2 then return nil end;return nil,why end
        local bytes=assert(f:read('*a'))
        assert(f:close())
        return bytes
    end
    -- Startup only: prepare known save directories before any UI callbacks can write.
    function F.mkdir(path)
        local probe=path..'/.directory.tmp'
        local f,why,code=io.open(probe,'wb')
        if not f then
            if code~=2 then return nil,why end
            if windows then os.execute('mkdir "'..native(path)..'" >nul 2>&1') else assert(os.execute('mkdir -p -- '..shell(path))) end
            f,why=io.open(probe,'wb');if not f then return nil,why end
        end
        assert(f:close());return os.remove(probe)
    end
    function F.write(path,bytes)
        local temporary=path..'.tmp'
        local f,why=io.open(temporary,'wb');if not f then return nil,why end
        assert(f:write(bytes))
        assert(f:flush())
        assert(f:close())
        -- Bundled Lua uses MoveFileExA(REPLACE_EXISTING | WRITE_THROUGH) on Windows.
        return os.rename(temporary,path)
    end
    function F.remove(path) return os.remove(path) end
    function F.list(path)
        local command=windows and ('dir /b /a "'..native(path)..'" 2>nul')
            or ('if [ -d '..shell(path)..' ]; then find '..shell(path).." -mindepth 1 -maxdepth 1 -printf '%f\\0'; fi")
        local pipe=assert(io.popen(command))
        local bytes=pipe:read('*a')
        pipe:close()
        local result={}
        for name in bytes:gmatch(windows and '[^\r\n]+' or '[^%z]+') do result[#result+1]=name end
        table.sort(result);return result
    end
    return F
end
