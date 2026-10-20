local function normal(p) return (p:gsub('\\','/'):gsub('/+$','')) end
local expected=normal(GateFixture)
local function metadata(tree)
    assert(type(tree)=='table' and tree.__name=='GameRoot','ROUND6 metadata root name')
    assert(normal(tree.__absolute_path)==expected,'ROUND6 metadata root path')
    assert(type(tree.__files)=='table' and tree.__missing==nil,'ROUND6 metadata root files/unknown')
    local dir=assert(tree.Game.Content.Paks.LogicMods)
    assert(dir.__name=='LogicMods' and normal(dir.__absolute_path)==expected..'/Game/Content/Paks/LogicMods','ROUND6 metadata child')
    local files=dir.__files;assert(#files==1,'ROUND6 dense file array')
    for i,file in pairs(files) do
        assert(i==1 and file.__name=='example.pak' and normal(file.__absolute_path)==expected..'/Game/Content/Paks/LogicMods/example.pak','ROUND6 file metadata')
    end
    for _,file in pairs(dir.Example.__files) do assert(file.__name=='config.lua') end
    assert(#dir.Empty.__files==0 and #dir.Empty.Deep.__files==0)
    assert(dir.__name=='LogicMods' and #dir.__files==1 and dir.__missing==nil,'ROUND6 repeated lookup')
    local unicode=assert(tree.Game.Binaries.Win64.ue4ss.Mods['\195\156nicodePackage'])
    assert(unicode.__name=='\195\156nicodePackage' and #unicode.__files==1,'ROUND6 Unicode metadata')
    local n=0;for key,node in pairs(tree.Game) do assert(type(key)=='string' and type(node)=='table');n=n+1 end;assert(n>=2)
end
local wrappers={'UObject','UClass','UStruct','UFunction','UEnum','UWorld','AActor','UInterface','UDataTable','FOutputDevice',
 'XProperty','XArrayProperty','XBoolProperty','XEnumProperty','XInterfaceProperty','XFieldClass','RemoteUnrealParam','LocalUnrealParam','XObjectProperty','XStructProperty','XDelegateProperty','XMulticastDelegateProperty','XMulticastSparseDelegateProperty',
 'FName','FText','FString','FUtf8String','FAnsiString','FURL','FWeakObjectPtr','TSoftObjectPtr','FSoftObjectPath','UScriptStruct','RemoteObject'}
local function types()
    assert(ModRef:type()=='ModRef')
    ModRef:SetSharedVariable('round6_value',42);assert(ModRef:GetSharedVariable('round6_value')==42)
    local t=GetCurrentThreadId();assert(t:type()=='ThreadId' and type(t:ToString())=='string' and t==GetCurrentThreadId())
    assert(type(GetMainModThreadId():ToString())=='string' and type(GetAsyncThreadId():ToString())=='string' and type(GetGameThreadId():ToString())=='string')
    assert(type(IsInGameThread())=='boolean' and type(IsInMainModThread())=='boolean' and type(IsInAsyncThread())=='boolean')
    local bad=CreateInvalidObject();assert(not bad:IsValid() and bad:GetAddress()==0 and bad:type()=='UObject')
    local a,b,c=UE4SS.GetVersion();assert(math.type(a)=='integer' and math.type(b)=='integer' and math.type(c)=='integer')
    local major,minor=UnrealVersion.GetMajor(),UnrealVersion.GetMinor()
    -- Uninitialized engine version is -1/-1. Compare against valid arguments;
    -- the SDK correctly rejects passing those negative sentinels back as uint32.
    local qm,qn=math.max(major,0),math.max(minor,0)
    local eq=major==qm and minor==qn
    local below=major<qm or (major==qm and minor<qn)
    local above=major>qm or (major==qm and minor>qn)
    assert(UnrealVersion.IsEqual(qm,qn)==eq and UnrealVersion.IsAtLeast(qm,qn)==not below and UnrealVersion.IsAtMost(qm,qn)==not above)
    assert(UnrealVersion.IsBelow(qm,qn)==below and UnrealVersion.IsAbove(qm,qn)==above)
    assert(FPackageName.IsShortPackageName('short') and not FPackageName.IsShortPackageName('/Game/Long'))
    assert(FPackageName.IsValidLongPackageName('/Game/Long'))
    for _,name in ipairs(wrappers) do
        -- Production userdata intentionally hides its metatable from getmetatable.
        local x=GateWrapper(name);local mt=debug.getmetatable(x)
        assert(type(x)=='userdata' and type(mt)=='table' and type(mt.__gc)=='function','ROUND6 wrapper metatable '..name)
        -- Read the real member table through each type's actual __index when provided.
        if mt.__index then
            local method=x.type;if type(method)=='function' then assert(type(x:type())=='string') end
        end
        if name=='FName' or name=='FText' then assert(not pcall(function() return x({}) end),'ROUND6 constructor argument path '..name) end
        if name=='FString' or name=='FUtf8String' or name=='FAnsiString' then assert(x:Len()==0 and x:IsEmpty() and x:ToString()=='') end
        if name=='RemoteObject' then assert(not x:IsValid()) end
    end
    collectgarbage('collect')
end
local function discovery()
    local scripts=expected..'/Game/Binaries/Win64/ue4ss/Mods/WitchItRuntime/Scripts/'
    local logs={};local folders=dofile(scripts..'discovery.lua')(scripts,function(s)logs[#logs+1]=s end)
    -- Stock Lua io.open uses the process ANSI code page on Windows. Keep
    -- Unicode metadata coverage, but do not assume UTF-8 CRT file opening.
    local unicode=io.open(expected..'/Game/Binaries/Win64/ue4ss/Mods/\195\156nicodePackage/manifest.lua','r')
    local count=unicode and 3 or 2;if unicode then unicode:close() end
    assert(#folders==count,'ROUND6 WIR discovery count expected '..count..', got '..#folders..'; '..table.concat(logs,'; '))
    assert(folders[1].folder=='ExamplePackage' and folders[2].folder=='SecondPackage','ROUND6 WIR safe package discovery')
    if count==3 then assert(folders[3].folder=='\195\156nicodePackage','ROUND6 WIR Unicode discovery') end
    assert(#logs>=2 and logs[#logs]=='discovered '..count..' package folders')
    if not discovery_capability_reported then
        print('ROUND6 NOTE stock Lua Unicode manifest io.open: '..(unicode and 'available' or 'unavailable (ANSI code page)'))
        discovery_capability_reported=true
    end
end
local function run()
    local tree=IterateGameDirectories();metadata(tree);types();discovery()
    -- First SDK operation in a previously unseen coroutine is the captured
    -- table's metadata lookup, followed by a yield and another Native entry.
    local co=coroutine.create(function()
        assert(tree.Game.__name=='Game','ROUND6 first coroutine metadata');metadata(tree)
        coroutine.yield('paused');metadata(IterateGameDirectories());types();discovery()
    end)
    local ok,value=coroutine.resume(co);assert(ok,value);assert(value=='paused')
    ok,value=coroutine.resume(co);assert(ok,value);assert(coroutine.status(co)=='dead')
    coroutine.wrap(function()metadata(tree);coroutine.wrap(function()metadata(IterateGameDirectories()) end)() end)()
    GateInvoke(function()coroutine.wrap(function()metadata(tree);types() end)() end)
    checks_count=(checks_count or 0)+1
end
local function unload()
    run()
    local file=assert(io.open(GateUnloadPath,'w'));assert(file:write('completed'));assert(file:close())
end
return {unload=unload,run=run,metadata=metadata,types=types,discovery=discovery}
