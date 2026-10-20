-- Shared scalar document ownership. No native publisher, filesystem, timer or RNG.
-- Only Core may admit/bind native or authoritative peer facts. Loading this
-- service establishes neither an owner nor a ready document.
local M={}
local MAX=2147483647
local function integer(v) return type(v)=='number' and v%1==0 and v>=0 and v<=MAX end
local function text(v,empty) return type(v)=='string' and #v<=4096 and (empty or #v>0) and not v:find('[%c]') end
local function flat(value,allowed)
    assert(type(value)=='table' and getmetatable(value)==nil,'plain scalar record required')
    local copy={}
    for k,v in pairs(value) do
        assert(allowed[k],'unknown document field')
        assert(type(v)=='string' and text(v,true) or type(v)=='boolean' or integer(v),'scalar document field required')
        copy[k]=v
    end
    return copy
end
local function copy(t) if not t then return nil end;local r={};for k,v in pairs(t) do r[k]=v end;return r end
local function equal(a,b)
    for k,v in pairs(a) do if b[k]~=v then return false end end
    for k,v in pairs(b) do if a[k]~=v then return false end end
    return true
end
function M.new(R,subscribe,call,Generations)
    local owner,document,origin
    local subscribers={}
    local captures=setmetatable({},{__mode='k'})
    local operations=setmetatable({},{__mode='k'})
    local acknowledgements=setmetatable({},{__mode='k'})
    local generation,publication,depth=0,0,0
    local record={};local pendingReason
    local B={}
    local function live(p,A) return R.is_healthy() and p.active and p.api==A end
    local function currentOwner()
        return owner and depth==0 and live(owner.p,owner.api) and not R.loading and not R.state_retired
            and owner.scope.world_generation==R.world_generation and owner.scope.world_key==R.world_key
    end
    local function snapshot()
        if not currentOwner() or not document then return nil end
        local s=copy(owner.scope)
        s.owner_generation=owner.generation;s.document_generation=document.generation
        s.revision=document.revision;s.publication_revision=publication
        for k,v in pairs(document.value) do s[k]=v end
        s.bound=s.document_kind=='saved'
        if origin then
            s.origin_world=origin.world;s.origin_name=origin.name;s.origin_revision=origin.revision
            s.origin_authority=owner.scope.role=='host' and 'local_gm' or 'host_peer'
        end
        return s
    end
    local function notify(reason)
        local ref=record
        for _,s in ipairs({table.unpack(subscribers)}) do
            if record~=ref or not R.is_healthy() then break end
            if s.alive and live(s.owner,s.api) and s.publication~=publication then
                s.publication=publication
                call(s.owner,'map document '..reason,s.fn,s,snapshot(),reason)
            end
        end
    end
    local function changed(reason,deliver)
        publication=publication+1;record={};pendingReason=reason
        if deliver~=false then pendingReason=nil;notify(reason) end
    end
    local function consumer(c,p,A)
        return c and c.p==p and c.api==A and live(p,A)
    end
    local function captureCurrent(c)
        return c and currentOwner() and c.owner==owner and c.document==document
            and c.revision==document.revision and live(c.p,c.api) or false
    end
    local function saveCurrent(op)
        return op and not op.closed and currentOwner() and op.owner==owner and op.document==document
            and op.serial==document.saveSerial and live(op.p,op.api) or false
    end
    local function operationAccess(op,p,A)
        return consumer(op,p,A) or op and live(p,A) and p.id=='Core'
            and currentOwner() and owner.p==p and owner.api==A
    end
    local function receipt(op)
        if not op.result then return nil end
        local r=copy(op.result)
        r.target=copy(op.target);r.captured_document=copy(op.captured)
        r.current_document=copy(op.completedDocument)
        return r
    end
    local function capture(p,A)
        local value=snapshot();if not value then return nil,'unbound' end
        local token={}
        captures[token]={p=p,api=A,owner=owner,document=document,revision=document.revision}
        return token,value
    end
    local function target(value)
        local v=flat(value,{world=true,name=true,target_path=true})
        assert(text(v.world) and text(v.name) and text(v.target_path),'literal save target required')
        assert(v.world==owner.scope.world,'save world mismatch')
        return v
    end
    local function saveState(op)
        return {kind=op.kind,operation_serial=op.serial,native_success=op.nativeSuccess,
            target=copy(op.target),document=copy(op.captured)}
    end
    local function invalidate(reason)
        owner=nil;document=nil;origin=nil;changed(reason,false)
    end
    function B.clear(reason,beforeNotify,pure)
        depth=depth+1
        invalidate(reason or 'owner cleared') -- revoke ALL tokens before outgoing code
        if beforeNotify then
            local ok,err=pcall(beforeNotify)
            if not ok then depth=depth-1;error(err,0) end
        end
        depth=depth-1
        if pure or not R.is_healthy() then subscribers={};pendingReason=nil
        else local why=pendingReason;pendingReason=nil;if why then notify(why) end end
    end
    function B.retire_key(key,reason,beforeNotify)
        if owner and owner.scope.actor_key==key then B.clear(reason,beforeNotify);return true end
        return false
    end
    function B.retire_package(p)
        if owner and owner.p==p then invalidate('document publisher disabled') end
    end
    function B.attach(p,A)
        local D={};A.map_document=D
        function D.capture()
            if not live(p,A) then return nil,'inactive' end
            return capture(p,A)
        end
        function D.current(token)
            local c=captures[token]
            return consumer(c,p,A) and captureCurrent(c) or false
        end
        -- Exact scalar lease for rollback while the consumer is quiesced.
        -- Never captures a new document or admits edits in this lane.
        function D.cleanup_current(token)
            local c=captures[token]
            if not c or c.p~=p or c.api~=A or p.api~=A or R.packages[p.id]~=p
                or not owner or not document or depth~=0 or R.loading or R.state_retired then return false end
            if not R.is_healthy() and not (R.health_state=='stopping' and R.unload_started==true) then return false end
            return owner.p.api==owner.api and R.packages[owner.p.id]==owner.p
                and c.owner==owner and c.document==document and c.revision==document.revision
                and owner.scope.world_generation==R.world_generation and owner.scope.world_key==R.world_key
        end
        -- ShipParts cleanup writes are independently restricted to native
        -- journal originals and exact weak object births. Permit their rollback
        -- across logical layout replacement in the SAME physical owner/world.
        if p.id=='BaseWorldPlus' then
            function D.rollback_current(token)
                local c=captures[token]
                return c and c.p==p and c.api==A and p.api==A and R.packages[p.id]==p
                    and owner and c.owner==owner and depth==0 and not R.loading and not R.state_retired
                    and (R.is_healthy() or R.health_state=='stopping' and R.unload_started==true)
                    and owner.p.api==owner.api and R.packages[owner.p.id]==owner.p
                    and owner.scope.world_generation==R.world_generation and owner.scope.world_key==R.world_key or false
            end
        end
        function D.on_change(fn,options)
            if not live(p,A) then return nil end
            assert(type(fn)=='function','document observer required')
            local cancel,s=subscribe(p,subscribers,fn);s.api=A;s.ungated=true
            if options and options.replay then
                s.publication=publication
                call(p,'map document replay',fn,s,snapshot(),'replay')
            end
            return cancel
        end
        function D.begin_save(token,kind)
            local c=captures[token]
            if not consumer(c,p,A) or not captureCurrent(c) then return nil,'stale' end
            assert(kind=='manual' or kind=='recovery','invalid save kind')
            if document.value.document_kind=='not_creative' then return nil,'not a Creative document' end
            if owner.scope.role~='host' then return nil,'not an authoritative document' end
            if document.saveSerial>=MAX then return nil,'operation serial exhausted' end
            document.saveSerial=document.saveSerial+1
            local handle={}
            operations[handle]={p=p,api=A,owner=owner,document=document,serial=document.saveSerial,
                kind=kind,captured=snapshot(),closed=false}
            return handle,saveState(operations[handle])
        end
        function D.save_current(handle)
            local op=operations[handle]
            return operationAccess(op,p,A) and saveCurrent(op) or false
        end
        function D.save_state(handle)
            local op=operations[handle]
            if not operationAccess(op,p,A) or not saveCurrent(op) then return nil end
            return saveState(op)
        end
        function D.save_ack(handle,success)
            local op=operations[handle]
            if not consumer(op,p,A) or not saveCurrent(op) or not op.nativeSeen or op.ack then return nil end
            assert(type(success)=='boolean','companion result required')
            local ack={}
            acknowledgements[ack]={operation=op,p=p,api=A,success=success,nativeSuccess=op.nativeSuccess,target=op.target,captured=op.captured}
            op.ack=ack
            return ack
        end
        function D.save_result(handle)
            local op=operations[handle]
            if not operationAccess(op,p,A) or not op.closed or not currentOwner() or op.owner~=owner
                or op.document~=document or op.serial~=document.saveSerial then return nil end
            return receipt(op)
        end
        if p.id=='Core' then
            local function publisher(token) return live(p,A) and currentOwner() and token==owner.token and owner.p==p and owner.api==A end
            function D.admit(value)
                if not live(p,A) or depth>0 or R.loading or R.state_retired then return nil end
                local s=flat(value,{role=true,world_generation=true,world_key=true,core_epoch=true,actor_key=true,world=true,peer_session=true})
                assert(s.role=='host' or s.role=='client','document role required')
                assert(integer(s.world_generation) and integer(s.core_epoch) and text(s.world_key)
                    and text(s.actor_key) and text(s.world),'admitted scalar owner required')
                assert(s.role~='client' or text(s.peer_session),'client peer session required')
                if s.world_generation~=R.world_generation or s.world_key~=R.world_key then return nil end
                local serial=Generations.next();if not serial then return nil end
                local token={}
                owner={p=p,api=A,scope=s,token=token,generation=serial,lastBinding=0,lastOrigin=0}
                document=nil;origin=nil;changed('owner admitted')
                if not publisher(token) then return nil end -- observer reentry superseded admission
                return token
            end
            function D.publish_origin(token,revision,value)
                if not publisher(token) then return false end
                assert(integer(revision) and revision>0,'origin revision required')
                local v=flat(value,{world=true,name=true})
                assert(text(v.world) and text(v.name,true) and v.world==owner.scope.world,'literal origin required')
                if revision<=owner.lastOrigin then return false end
                owner.lastOrigin=revision;origin={world=v.world,name=v.name,revision=revision}
                local ref=owner;changed('origin published');return owner==ref and publisher(token) or false
            end
            function D.bind(token,revision,value,event)
                if not publisher(token) then return false end
                assert(integer(revision) and revision>0,'document revision required')
                local v=flat(value,{document_kind=true,world=true,name=true,target_path=true,source_generation=true,source_revision=true})
                assert(v.document_kind=='saved' or v.document_kind=='unsaved' or v.document_kind=='not_creative','document kind required')
                assert(text(v.world) and v.world==owner.scope.world,'document world mismatch')
                assert(v.document_kind=='saved' and text(v.name) or v.document_kind~='saved' and v.name==nil and v.target_path==nil,'explicit name-unbound document required')
                if owner.scope.role=='host' then
                    assert(event=='load' or event=='new' or event=='copy' or event=='world_rules','explicit host binding event required')
                    assert(event~='new' or v.document_kind=='unsaved','NewLayout must bind an unsaved document')
                    assert((event=='world_rules')==(v.document_kind=='not_creative'),'explicit non-Creative rules scope required')
                    assert(v.document_kind~='saved' or text(v.target_path),'literal host document target required')
                    assert(v.source_generation==nil and v.source_revision==nil,'host document cannot borrow peer revisions')
                else
                    assert(event=='host_publication' and v.target_path==nil,'client identity requires central host publication')
                    assert(integer(v.source_generation) and v.source_generation>0 and integer(v.source_revision) and v.source_revision>0,'host source identity required')
                    if document then
                        local old=document.value
                        if v.source_generation<old.source_generation or v.source_generation==old.source_generation and v.source_revision<old.source_revision then return false end
                        if v.source_generation==old.source_generation and v.source_revision==old.source_revision then return equal(v,old) end
                    end
                end
                if revision<=owner.lastBinding or generation>=MAX then return false end
                generation=generation+1;owner.lastBinding=revision
                document={generation=generation,revision=revision,value=v,saveSerial=0}
                local ref=document;changed(event);return document==ref and publisher(token) or false
            end
            function D.release(token,reason)
                if not publisher(token) then return false end
                B.clear(reason or 'owner released');return true
            end
            -- A successful native file write changes the working identity even
            -- if any independent observer cannot publish its companion.
            function D.file_saved(token,value)
                if not publisher(token) or owner.lastBinding>=MAX or owner.lastOrigin>=MAX then return false end
                local v=target(value)
                if not document then
                    if generation>=MAX then return false end
                    generation=generation+1;document={generation=generation,saveSerial=0}
                end
                owner.lastBinding=owner.lastBinding+1;document.revision=owner.lastBinding
                document.value={document_kind='saved',world=v.world,name=v.name,target_path=v.target_path}
                owner.lastOrigin=owner.lastOrigin+1;origin={world=v.world,name=v.name,revision=owner.lastOrigin}
                changed('native manual target');return true
            end
            function D.save_native(handle,success,value)
                local op=operations[handle]
                if not live(p,A) or not saveCurrent(op) or owner.p~=p or owner.api~=A or op.nativeSeen then return false end
                assert(type(success)=='boolean','actual writer boolean required')
                local v=success and target(value) or nil
                if success and op.kind=='manual' and (owner.lastBinding>=MAX or owner.lastOrigin>=MAX) then return false end
                op.nativeSeen=true;op.nativeSuccess=success;op.target=v
                if success and op.kind=='manual' then
                    -- Actual .pl success updates working target/raw origin even if
                    -- later geometry metadata fails. This is NOT a geometry commit.
                    owner.lastBinding=owner.lastBinding+1;document.revision=owner.lastBinding
                    document.value={document_kind='saved',world=v.world,name=v.name,target_path=v.target_path}
                    owner.lastOrigin=owner.lastOrigin+1;origin={world=v.world,name=v.name,revision=owner.lastOrigin}
                    changed('native manual target')
                end
                return saveCurrent(op)
            end
            function D.complete_save(handle,ack)
                local op=operations[handle]
                if not live(p,A) or not saveCurrent(op) or owner.p~=p or owner.api~=A then return false end
                local a=acknowledgements[ack]
                if not op.nativeSeen or not a or op.ack~=ack or a.operation~=op
                    or a.p~=op.p or a.api~=op.api or not live(a.p,a.api)
                    or a.nativeSuccess~=op.nativeSuccess or a.target~=op.target or a.captured~=op.captured then return false end
                local companionSuccess=a.success
                local success=op.nativeSuccess==true and companionSuccess
                op.closed=true
                op.result={success=success,aggregate_success=success,kind=op.kind,operation_serial=op.serial,
                    native_success=op.nativeSuccess,companion_success=companionSuccess,
                    owner_generation=owner.generation,document_generation=document.generation,revision=document.revision}
                op.completedDocument=snapshot()
                -- Dirty/committed geometry belongs to the participant kernel,
                -- which publishes only this successful receipt's captured edit.
                local token,view=capture(op.p,op.api)
                return success,token,view,receipt(op)
            end
        end
        A.own(function()
            if owner and owner.p==p and owner.api==A then B.retire_package(p) end
            local why=pendingReason;pendingReason=nil;if why then notify(why) end
        end,function()
            if owner and owner.p==p and owner.api==A then B.retire_package(p) end
        end)
    end
    return B
end
return M
