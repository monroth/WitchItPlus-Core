-- Bounded data files under ProjectSavedDir/WitchItRuntime/<package-id>.
local dir=assert(debug.getinfo(1,'S').source:match('^@(.+[\\/])'))
local S={json=dofile(dir..'json.lua')}
function S.clone(value)
    if type(value)~='table' then return value end
    local result={}; for k,v in pairs(value) do result[k]=S.clone(v) end
    return setmetatable(result,getmetatable(value))
end
function S.merge(target,source)
    assert(type(source)=='table','config must be a JSON object')
    for k,v in pairs(source) do
        if type(v)=='table' and type(target[k])=='table' then S.merge(target[k],v)
        else target[k]=S.clone(v) end
    end
    return target
end
function S.name(name)
    assert(type(name)=='string' and #name>=1 and #name<=96 and name:match('^[%w_%-][%w_.%-]*$')
        and not name:find('..',1,true) and not name:match('[.]$'),'invalid filename')
    local base=name:match('^[^.]+'):upper()
    assert(not ({CON=true,PRN=true,AUX=true,NUL=true})[base] and not base:match('^COM[1-9]$')
        and not base:match('^LPT[1-9]$'),'reserved filename')
    return name
end
-- A successful marker probe proves the directory exists without a shell/process.
local function probe(path)
    local marker=path..'/.wir-directory'
    local f=io.open(marker,'rb')
    if f then f:close(); return true end
    f=io.open(marker,'wb')
    if f then f:write('WIR1\n'); f:close(); return true end
    return false
end
local attempted={}
function S.provision(paths,allowShell)
    local missing,seen={},{}
    for _,path in ipairs(paths) do
        assert(type(path)=='string' and not path:find('[%z\r\n]'),'invalid directory')
        if not seen[path] and not probe(path) then missing[#missing+1]=path end
        seen[path]=true
    end
    if #missing==0 then return end
    assert(allowShell,'directory missing; run WitchItRuntime/provision.py before launching: '..missing[1])
    local args={}
    for _,path in ipairs(missing) do
        assert(not attempted[path],'directory provisioning already attempted: '..path)
        attempted[path]=true
        if package.config:sub(1,1)=='\\' then
            assert(not path:find('["%%!&|<>^]'),'unsupported saved path characters')
            args[#args+1]='"'..path:gsub('/','\\')..'"'
        else args[#args+1]="'"..path:gsub("'","'\\''").."'" end
    end
    -- Installation bootstrap only, one process for all missing directories.
    -- Existing installs never execute this path; gameplay callers cannot enable it.
    local command=(package.config:sub(1,1)=='\\' and 'mkdir ' or 'mkdir -p -- ')..table.concat(args,' ')
    local ok,_,code=os.execute(command)
    assert(ok==true or ok==0 or code==0,'cannot provision settings directories')
    for _,path in ipairs(missing) do assert(probe(path),'cannot write directory marker: '..path) end
end
function S.mkdir(path) return S.provision({path},false) end
function S.read(path)
    local f,err=io.open(path,'rb'); if not f then return nil,err end
    local data=assert(f:read(262145)); assert(f:close()); assert(#data<=262144,'file too large'); return data
end
function S.atomic(path,bytes)
    assert(type(bytes)=='string' and #bytes<=262144,'file too large')
    local f=assert(io.open(path..'.tmp','wb')); assert(f:write(bytes)); assert(f:flush()); assert(f:close())
    local previous=S.read(path)
    if previous then
        os.remove(path..'.bak'); assert(os.rename(path,path..'.bak'),'backup failed')
    end
    local ok,err=os.rename(path..'.tmp',path)
    if not ok then
        if previous then assert(os.rename(path..'.bak',path),'restore failed') end
        os.remove(path..'.tmp'); error(err)
    end
    return true
end
-- Resumable atomic small-file write for consumers with a frame budget. Keep
-- the same backup/rollback contract as atomic(); the caller owns the finite step
-- and must finish or abort it before releasing its package scope.
function S.atomic_steps(path,bytes)
    assert(type(bytes)=='string' and #bytes<=262144,'file too large')
    local phase,f,old,previous,backed,committed=0,nil,nil,false,false,false
    local function abort()
        if f then pcall(function() f:close() end);f=nil end
        if old then pcall(function() old:close() end);old=nil end
        if backed and not committed then assert(os.rename(path..'.bak',path),'restore failed') end
        if not committed then os.remove(path..'.tmp') end
    end
    local function step()
        if committed then return true end
        phase=phase+1
        local ok,err=pcall(function()
            if phase==1 then f=assert(io.open(path..'.tmp','wb'))
            elseif phase==2 then assert(f:write(bytes))
            elseif phase==3 then assert(f:flush())
            elseif phase==4 then assert(f:close());f=nil
            elseif phase==5 then old=io.open(path,'rb');previous=old~=nil
            elseif phase==6 then if old then assert(old:close());old=nil end
            elseif phase==7 then if previous then os.remove(path..'.bak') end
            elseif phase==8 then if previous then assert(os.rename(path,path..'.bak'),'backup failed');backed=true end
            else assert(os.rename(path..'.tmp',path),'commit failed');committed=true end
        end)
        if not ok then abort();error(err) end
        return committed
    end
    return step,abort
end
function S.new(root,mkdir)
    (mkdir or S.mkdir)(root)
    local A={root=root}
    function A.path(name) return root..'/'..S.name(name) end
    function A.read(name) return S.read(A.path(name)) end
    function A.write(name,bytes) return S.atomic(A.path(name),bytes) end
    function A.read_json(name)
        local text=A.read(name); if text then return S.json.decode(text) end
        local backup=S.read(A.path(name)..'.bak'); if backup then return S.json.decode(backup) end
    end
    function A.write_json(name,value) return A.write(name,S.json.encode(value)) end
    function A.write_json_steps(name,value) return S.atomic_steps(A.path(name),S.json.encode(value)) end
    return A
end
return S
