-- Central current document binding from the native GM and actual file observations.
local M={}
local CREATIVE='/Game/Blueprints/Gamemodes/CreativeMode/B_CreativeGameMode.B_CreativeGameMode_C'
local CLASSES={B_CreativeGameMode_C=CREATIVE,
    B_SeekGameMode_C='/Game/Blueprints/Core/B_SeekGameMode.B_SeekGameMode_C',
    B_ObjectiveGameMode_C='/Game/Blueprints/Gamemodes/ObjectiveMode/B_ObjectiveGameMode.B_ObjectiveGameMode_C',
    B_ImpostorGameMode_C='/Game/Blueprints/Gamemodes/ImpostorMode/B_ImpostorGameMode.B_ImpostorGameMode_C'}
local MAX=2147483647
local function text(x,empty) return type(x)=='string' and #x<=4096 and (empty or #x>0) and not x:find('[%c]') end
local function copy(x) if not x then return end;local y={};for k,v in pairs(x) do y[k]=v end;return y end
function M.new(api,Core)
    assert(api.id=='Core' and api.map_document,'central document owner requires new Core API')
    local D=api.map_document
    local C=Core.new(api,{native_travel_hooks=false,budget={ms=6,reserve=3,items=8}})
    local H,L=C.runtime,C.lifetime
    local closed,record,serial,mutation,scanKey,scanJob,scanSerial=false,nil,0,0,nil,nil,0
    local scanned=false;local P={};local nativeLeases=setmetatable({},{__mode='k'})
    local function live(epoch,stamp)
        local R=WIR_RUNTIME
        return not closed and api.is_live() and api.packages.owner_current() and api.context().mode~='menu' and not L.transition and L.epoch==epoch and mutation==stamp
            and R and R.alive==true and not R.loading and not R.state_retired
    end
    local function nativeScope(value,role,peerSession,held)
        local epoch,stamp=L.epoch,mutation;local R=WIR_RUNTIME;local generation=R and R.world_generation
        if not live(epoch,stamp) or held and not held()then return end
        local actor=Core.runtime.unwrap(value)
        if not actor or not actor:IsValid() or actor:IsActorBeingDestroyed()then return end
        -- Authority is an Actor fact. A solo listen server is still the host.
        if actor:HasAuthority()~=(role=='host')then return end
        local name=actor:GetFullName();if not text(name)or name:find('Default__',1,true)then return end
        local kind=name:match('^(%S+)');if role=='host'and not CLASSES[kind]then return end
        local world=actor:GetWorld();if not world or not world:IsValid()then return end
        local worldKey=tostring(world:GetAddress())..'|'..world:GetFullName()
        if worldKey~=R.world_key then return end
        local actorKey=tostring(actor:GetAddress())..'|'..name
        if L.retired[actorKey:gsub('|',':',1)]then return end
        if role=='client'then
            local input=R.input_owner
            if not actor:IsLocalPlayerController()or not input or input.key~=actorKey
                or input.world~=worldKey or input.generation~=R.world_generation then return end
        end
        local statics=StaticFindObject('/Script/Engine.Default__GameplayStatics')
        if not statics or not statics:IsValid()then return end
        local level=Core.runtime.text(statics:GetCurrentLevelName(actor,true))
        if not text(level)or not live(epoch,stamp)or held and not held()then return end
        if L.retired[actorKey:gsub('|',':',1)]or not actor:IsValid()or actor:IsActorBeingDestroyed()
            or R.world_key~=worldKey or R.world_generation~=generation then return end
        return {role=role,world_generation=R.world_generation,world_key=worldKey,core_epoch=epoch,
            actor_key=actorKey,world=level,peer_session=peerSession},kind,stamp,actor
    end
    local function nextRevision()
        if serial>=MAX then return end;serial=serial+1;return serial
    end
    local function adopt(actor,scope,kind,stamp)
        if not live(scope.core_epoch,stamp)or L.retired[scope.actor_key:gsub('|',':',1)]then return end
        local old=record
        if old and old.scope.actor_key==scope.actor_key and old.scope.world_key==scope.world_key
            and old.scope.world_generation==scope.world_generation and old.scope.peer_session==scope.peer_session
            and old.scope.role==scope.role and old.scope.core_epoch==scope.core_epoch then return old end
        mutation=mutation+1;local currentStamp=mutation
        -- Admission can synchronously notify ordinary consumers. Publish no stale
        -- owner after reentrant travel/replacement/terminal teardown.
        record=nil
        local token=D.admit(scope)
        if not token or not live(scope.core_epoch,currentStamp)or L.retired[scope.actor_key:gsub('|',':',1)]then return end
        record={token=token,scope=scope,kind=kind,actor=actor,stamp=currentStamp}
        return record
    end
    local function current(r) return r and record==r and live(r.scope.core_epoch,r.stamp) end
    function P.observe_host(actor,newLayout)
        local scope,kind,stamp,unwrapped=nativeScope(actor,'host')
        actor=unwrapped
        if not scope or not CLASSES[kind] then return false end -- unknown subclass is unavailable
        local r=adopt(actor,scope,kind,stamp);if not current(r) then return false end
        if kind=='B_CreativeGameMode_C' then
            local G=Core.guarded_native.new(api,L,function()return current(r)end)
            local name=G.text(G.read(r.actor,'LoadedLayoutName'))
            if not G.current() or not text(name,true) then return false end
            local rev=nextRevision();if not rev then return false end
            D.publish_origin(r.token,rev,{world=scope.world,name=name})
            if not current(r) then return false end
            if newLayout==true then
                Core.layout_files.clear(scope.actor_key)
                local binding=nextRevision();if not binding then return false end
                return D.bind(r.token,binding,{document_kind='unsaved',world=scope.world},'new') and current(r) or false
            end
            local file=Core.layout_files.current(scope.actor_key)
            local path=G.text(G.read(r.actor,'LoadedLayoutFullPath'))
            if not G.current() then return false end
            if text(name) and not name:find('[/\\]') and text(path) then
                path=path:gsub('\\','/')
                local directory,stem=path:match('^(.*[/])([^/]+)%.pl$')
                -- Vanilla may leave the autosave path in the GM. The authored
                -- document remains its manual name, as in the game's editor.
                if directory and stem==name..'_AutoSave' then path=directory..name..'.pl';stem=name end
                if directory and stem==name and (path:sub(1,1)=='/' or path:match('^%a:/')) then
                    file={kind='current',phase='current',success=true,owner_key=scope.actor_key,
                        world=scope.world,name=name,target_path=path}
                end
            end
            if file and file.success and file.name==name then
                local _,view=D.capture()
                if newLayout=='load' or not view or view.name~=name or view.target_path~=file.target_path then
                    local binding=nextRevision();if not binding then return false end
                    if not D.bind(r.token,binding,{document_kind='saved',world=scope.world,name=name,target_path=file.target_path},'load')
                        or not current(r) then return false end
                end
                Core.layout_files.refresh(file)
            end
            return current(r) and true or false
        end
        if newLayout then return false end
        local token,snapshot=D.capture()
        if token and D.current(token) and snapshot.document_kind=='not_creative' and snapshot.actor_key==scope.actor_key then return true end
        local rev=nextRevision();if not rev then return false end
        return D.bind(r.token,rev,{document_kind='not_creative',world=scope.world},'world_rules') and current(r) or false
    end
    function P.withdraw_peer(peerSession)
        local r=record
        if not current(r) or r.scope.role~='client' or r.scope.peer_session~=peerSession then return false end
        mutation=mutation+1;record=nil -- revoke private rights before owned consumer callbacks
        return D.release(r.token,'authoritative document unavailable')
    end
    function P.bind_peer(peerSession,fact,publicationCurrent)
        if publicationCurrent and not publicationCurrent() then return false end
        if type(fact)=='table' and fact.kind=='unavailable' then return P.withdraw_peer(peerSession) end
        if not text(peerSession) or type(fact)~='table' or fact.kind=='unavailable' then return false end
        -- A completed authoritative publication replaces the old fact even if
        -- its literal scope cannot be admitted. Capture exact private ownership;
        -- an older native walk must never release a reentrant successor.
        local entry,entrySerial,entryMutation=record,serial,mutation
        local function accepted() return not publicationCurrent or publicationCurrent() end
        local function scopeCurrent()
            return accepted() and record==entry and serial==entrySerial and mutation==entryMutation
        end
        local function reject(r,revision,stamp)
            if accepted() and r and record==r and serial==revision and mutation==stamp
                and r.scope.role=='client' and r.scope.peer_session==peerSession then
                mutation=mutation+1;record=nil
                D.release(r.token,'authoritative document scope refused')
            end
            return false
        end
        local R=WIR_RUNTIME;local pc=R and R.input and R.input.controller
        local ok,scope,_,stamp=pcall(nativeScope,pc,'client',peerSession,scopeCurrent)
        if not ok then pcall(reject,entry,entrySerial,entryMutation);error(scope,0) end
        if not scope or scope.world~=fact.world then return reject(entry,entrySerial,entryMutation) end
        if not scopeCurrent() then return false end
        local r=adopt(pc,scope,'client',stamp);if not current(r) then return false end
        if not accepted() then return false end
        local rev=nextRevision();if not rev then return reject(r,serial,mutation) end
        local bindingStamp=mutation
        local bound=D.bind(r.token,rev,{document_kind=fact.kind,world=fact.world,name=fact.name,
            source_generation=fact.source_generation,source_revision=fact.source_revision},'host_publication')
        if not bound then return reject(r,rev,bindingStamp) end
        return accepted() and current(r) or false
    end
    function P.file_saved(e)
        local r=record
        if not current(r) or r.scope.role~='host' or r.scope.actor_key~=e.owner_key then return false end
        local ok=D.file_saved(r.token,{world=r.scope.world,name=e.name,target_path=e.target_path})
        local token,v=D.capture();if token and type(v)=='table' then serial=math.max(serial,v.revision or 0,v.origin_revision or 0)end
        return ok
    end
    function P.capture()
        if not current(record) then return end
        local token,snapshot=D.capture()
        if not current(record) or not token or not D.current(token) or snapshot.actor_key~=record.scope.actor_key
            or snapshot.core_epoch~=record.scope.core_epoch or snapshot.peer_session~=record.scope.peer_session then return end
        return token,snapshot
    end
    function P.current(token) return current(record) and D.current(token) end
    local function clear()
        mutation=mutation+1;record=nil;nativeLeases=setmetatable({},{__mode='k'});scanned=false;scanKey=nil
    end
    L.onReset(clear)
    L.onRetired(function()
        local r=record
        if r and L.retired[(r.scope.actor_key:gsub('|',':',1))] then clear() end
    end)
    local function start()
        if scanned or closed or not api.is_live() or not api.packages.owner_current() or L.transition or not L.world then return end
        if scanKey and C.work_budget.keys[scanKey]==scanJob then return end
        local epoch=L.epoch;scanSerial=scanSerial+1;local key='document-bootstrap:'..scanSerial;scanKey=key
        local steps={};local classes={};for short in pairs(CLASSES) do classes[#classes+1]=short end;table.sort(classes)
        for _,class in ipairs(classes) do
            local rows
            steps[#steps+1]=C.work_budget.native(function() rows=api.find_all(class) end)
            steps[#steps+1]=C.work_budget.defer(function() return C.work_budget.each(rows or {},function(actor)
                if not closed and api.is_live() and L.epoch==epoch and not L.transition then P.observe_host(actor) end
            end) end)
        end
        steps[#steps+1]=function() scanned=true;if scanKey==key then scanKey=nil end;return true end
        local step=C.work_budget.sequence(steps)
        local stepFn=function()
            if closed or not api.is_live() or L.epoch~=epoch or L.transition then if scanKey==key then scanKey=nil end;return true end
            return step()
        end
        local ok,accepted=pcall(C.work_budget.submit,key,'finite native document owner admission',stepFn)
        if not ok then
            local job=C.work_budget.keys[key]
            if job and job.step==stepFn then pcall(C.work_budget.cancel,key) end
            if scanKey==key then scanKey=nil end;error(accepted,0)
        end
        if accepted==false and scanKey==key then scanKey=nil end
        if scanKey==key then scanJob=C.work_budget.keys[key] end
    end
    api.packages.observe(function(snapshot)
        if snapshot and snapshot.valid and api.packages.owner_current() and api.packages.is_current(snapshot.revision) then start() end
    end)
    api.on('world_ready',function(actor)
        P.observe_host(actor);start()
    end,{ungated=true})
    api.on('begin_play_post',function(actor) P.observe_host(actor) end,{ungated=true,class_filter=function(name)return CLASSES[name]~=nil end})
    local E=api.Native.scope(api.id)
    -- Cooked LoadLayout sets LoadedLayoutName/FullPath AFTER LoadPropLayout
    -- returns. Observe that actual outer function, rather than guessing a delay.
    E.on_call(CREATIVE..':LoadLayout','post',function(actor)P.observe_host(actor,'load')end)
    E.on_call(CREATIVE..':NewLayout','post',function(actor)P.observe_host(actor,true)end)
    api.own(function() closed=true;clear();return true end,function() closed=true;clear();return true end)
    api.own(Core.layout_files.observe(function(e)
        if not e.complete or not e.success or e.autosave or e.recovery then return end
        for _,actor in ipairs(api.find_all('B_CreativeGameMode_C')or {})do
            P.observe_host(actor)
        end
    end),function()return true end)
    start()
    return P
end
return M
