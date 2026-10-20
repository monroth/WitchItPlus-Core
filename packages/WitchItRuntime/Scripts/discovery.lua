-- One startup traversal; no shell, directory polling or package registry to maintain.
return function(scripts,log)
    local function normal(p) return p:gsub('\\','/'):gsub('/+$',''):lower() end
    local mods=normal(assert(scripts:match('^(.*)[\\/]WitchItRuntime[\\/]Scripts[\\/]?$'),
        'expected Mods/WitchItRuntime/Scripts layout'))
    local function find(node)
        if type(node)~='table' then return end
        if node.__absolute_path and normal(node.__absolute_path)==mods then return node end
        for k,v in pairs(node) do
            if k:sub(1,2)~='__' and type(v)=='table' then
                local found=find(v); if found then return found end
            end
        end
    end
    local parent=assert(find(assert(IterateGameDirectories(),'game directories unavailable')),
        'runtime Mods directory absent from IterateGameDirectories')
    local result={}
    for folder,node in pairs(parent) do
        if type(node)=='table' and folder:sub(1,2)~='__' then
            local path=node.__absolute_path
            if path then
                local file=io.open(path..'/manifest.lua','r')
                if file then
                    file:close()
                    local unsafe=false
                    for name in pairs(node) do
                        if name:lower()=='scripts' or name:lower()=='dlls' then unsafe=true end
                    end
                    local marker=io.open(path..'/enabled.txt','r')
                    if marker then marker:close(); unsafe=true end
                    if unsafe then log('discovery rejected '..folder..': package contains Scripts/dlls/enabled.txt')
                    else result[#result+1]={folder=folder,path=path} end
                end
            end
        end
    end
    table.sort(result,function(a,b) return a.folder<b.folder end)
    log('discovered '..#result..' package folders')
    return result
end
