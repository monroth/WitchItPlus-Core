-- Package globals are private. Native globals and inherited library tables are
-- read-only views; this is an accidental-write guard, not a security sandbox.
local realG=_G
local rawget,rawset,next,pairs,rawequal=rawget,rawset,next,pairs,rawequal
local getmetatable,setmetatable=getmetatable,setmetatable
local M={}
local critical={
    'tostring','type','select','pcall','xpcall','print','RegisterHook','UnregisterHook','NotifyOnNewObject','RegisterKeyBind',
    'RegisterCustomEvent','UnregisterCustomEvent','CancelDelayedAction','RegisterKeyBindAsync',
    'LoopAsync','ExecuteAsync','ExecuteInEngineTick','LoopInGameThread','RegisterConsoleCommandHandler','ExecuteWithDelay','ExecuteInGameThread',
    'ExecuteInGameThreadWithDelay','LoopInGameThreadWithDelay','ExecuteInGameThreadAfterFrames',
    'IsInGameThread','IsInAsyncThread','StaticFindObject',
    'FindAllOf','StaticConstructObject','UObject','AActor','FName','FText',
    'Key','ModifierKey','EObjectFlags','EInternalObjectFlags','PropertyTypes',
    'RegisterBeginPlayPreHook','RegisterBeginPlayPostHook','RegisterEndPlayPreHook',
    'RegisterEndPlayPostHook','RegisterInitGameStatePostHook','RegisterLoadMapPreHook',
    'RegisterLoadMapPostHook','AsyncThread','MainThread','HookThread',
}
local reserved={}
for _,key in ipairs(critical) do reserved[key]=true end
function M.new(id,packagePrint,moduleLoader,nativeWrapper)
    local nativeLoadfile,nativeLoad=realG.loadfile,realG.load
    local values,views,protected={},{},{[realG]=true}
    local environment
    local function denied(key) error('package '..id..' cannot write UE4SS/global table: '..tostring(key),3) end
    local function view(value)
        if type(value)~='table' or rawequal(value,rawget(realG,'WIR_RUNTIME')) then return value end
        if views[value] then return views[value] end
        local proxy={}; views[value]=proxy; protected[value]=true; protected[proxy]=true
        return setmetatable(proxy,{
            __index=function(_,key)
                if (rawequal(value,realG.os) and key=='execute') or (rawequal(value,realG.io) and key=='popen') then
                    return function() error('package '..id..' cannot spawn a process; use runtime storage',2) end
                end
                return view(value[key])
            end,
            __newindex=function(_,key) denied(key) end,
            __len=function() return #value end,
            __pairs=function()
                local key
                return function() key=next(value,key); if key~=nil then return key,view(value[key]) end end
            end,
            __call=function(_,...) return value(...) end,
            __metatable=false,
        })
    end
    local function assign(key,value)
        if key=='_G' or reserved[key] then denied(key) end
        values[key]=value
    end
    environment=setmetatable({}, {
        __index=function(_,key)
            if values[key]~=nil then return values[key] end
            local value=realG[key]
            if nativeWrapper and type(value)=='function' then value=nativeWrapper(key,value) end
            return view(value)
        end,
        __newindex=function(_,key,value) assign(key,value) end,
        __pairs=function() return next,values,nil end,
        __metatable=false,
    })
    values._G=environment
    values.print=packagePrint
    values.require=moduleLoader
    values.rawset=function(object,key,value)
        if rawequal(object,environment) then assign(key,value); return object end
        if protected[object] then denied(key) end
        return rawset(object,key,value)
    end
    values.rawget=function(object,key)
        if rawequal(object,environment) then return values[key] end
        if protected[object] then return object[key] end
        return rawget(object,key)
    end
    values.setmetatable=function(object,meta)
        if rawequal(object,environment) or protected[object] then denied('metatable') end
        return setmetatable(object,meta)
    end
    local safeDebug={}
    for _,key in ipairs({'traceback','getinfo'}) do safeDebug[key]=debug[key] end
    values.debug=view(safeDebug) -- no registry/uservalue/upvalue mutation escape
    local function target(env)
        if env==nil or rawequal(env,environment) then return environment end
        assert(type(env)=='table' and getmetatable(env)==nil and next(env)==nil,
            'package loader requires its own or empty data environment')
        return env
    end
    values.loadfile=function(path,mode,env) return nativeLoadfile(path,mode or 't',target(env)) end
    values.load=function(source,name,mode,env) return nativeLoad(source,name,mode or 't',target(env)) end
    values.dofile=function(path) return assert(values.loadfile(path))() end
    return environment
end
return M
