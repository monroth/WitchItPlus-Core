-- WIR2: connection compatibility is the acknowledged public package inventory.
-- Layout facts are replicated separately and never admit feature RPCs.
-- No periodic discovery, resend, warning deadline or silence-based revocation.
local M={protocol_version=2}
local Public=dofile(assert(debug.getinfo(1,'S').source:match('^@(.+[\\/])'))..'public_packages.lua')
local PC='/Game/Blueprints/Player/B_PlayerController.B_PlayerController_C:'
local PATHS={PC..'ExecuteConsoleCommandFromClient',PC..'ClientSetRevisionNumber'}
local PREFIX='WIR2:'
local MAX=2147483647
local function integer(x) return type(x)=='number' and x>=0 and x<=MAX and x%1==0 end
local function hex(s) return (s:gsub('.',function(c) return string.format('%02x',c:byte()) end)) end
local function unhex(s) return (s:gsub('%x%x',function(c) return string.char(tonumber(c,16)) end)) end
local function session(s)
    if type(s)~='string' then return end
    local boot,raw=s:match('^([0-9a-f]+)%-(%d+)$');local generation=tonumber(raw)
    if boot and #boot==16 and raw and #raw<=10 and integer(generation) and generation>0 and tostring(generation)==raw then return boot,generation end
end
local function inventoryShape(rows)
    if type(rows)~='table' or #rows>128 then return false end
    local n,count=#rows,0
    for key in pairs(rows) do
        if type(key)~='number' or key%1~=0 or key<1 or key>n then return false end
        count=count+1
    end
    return count==n
end
local function randomBoot()
    local bytes={};for i=1,8 do bytes[i]=string.char(math.random(0,255)) end
    return hex(table.concat(bytes)) -- probabilistic nonce, not global ordering/authentication
end
function M.new(api,Core,DocumentOwner)
    assert(DocumentOwner and api.map_document,'WIR2 requires central document ownership')
    assert(type(api.allocate_owner_generation)=='function' and type(api.hooks)=='table',
        'WIR2 requires owned generations and committed hook readiness')
    assert(type(api.retirement)=='table','WIR2 requires the healthy WIR-owned retirement lane')
    local C=Core.new(api,{native_travel_hooks=false,budget={ms=6,reserve=3,items=8}})
    local H,L=C.runtime,C.lifetime
    local registry=Core.registry.new(api,L,{
        B_PlayerController_C='/Game/Blueprints/Player/B_PlayerController.B_PlayerController_C',
        B_GameInstance_C='/Game/Blueprints/Core/B_GameInstance.B_GameInstance_C',
    },{persistent={B_GameInstance_C=true},inherited={B_PlayerController_C=true},ready_events=false,validate_list=false})
    local peers,queue,observers={},{},{}
    local intents,intentOrder={},{} -- at most four finite accepted publications per admitted route
    local endpoint,inventory,hookProof,localOwner=nil,nil,nil,nil
    local revision,dispatchSerial,dispatchKey,dispatchJob,transferSerial=0,0,nil,nil,0
    local complete,bootstrapKey,bootstrapJob,bootstrapSerial=false,nil,nil,0
    local boot=randomBoot();local closed=false;local overflow=false;local pendingNotification
    local S={protocol_version=2}
    local publish,reconcile,ensureSend,announce,mountServer,fillQueue,refreshCompatible
    local lastDocument
    local function live(epoch) return not closed and api.is_live() and not L.transition and L.epoch==epoch end
    local function ownerLive(owner) return type(owner)=='table' and type(owner.is_live)=='function' and owner.is_live()==true end
    local function nativeWalk(epoch,held)
        local stamp=revision
        return Core.guarded_native.new(api,L,function() return live(epoch) and revision==stamp and (not held or held()) end)
    end
    local function inventoryOwnsCore()
        if not inventory or not inventory.valid or not inventory.started then return false end
        for _,r in ipairs(inventory.packages or {}) do
            if r.id=='Core' then return r.version==Core.version and r.protocol_version==2 and r.required_on_all_players==true end
        end
        return false
    end
    local function endpointCurrent(e)
        return e and endpoint==e and live(e.epoch) and api.context().mode~='menu' and hookProof and hookProof.ready==true
            and hookProof.healthy==true and inventoryOwnsCore() and api.packages.owner_current() and api.packages.is_current(inventory.revision)
            and e.inventoryRevision==inventory.revision and localOwner and e.key==localOwner.key
    end
    local function documentShape(d)
        if type(d)~='table' then return false end
        local fields={kind=true,world=true,name=true,source_generation=true,source_revision=true}
        for k in pairs(d) do if not fields[k] then return false end end
        if d.kind=='unavailable' then return next(d,'kind')==nil and next(d)== 'kind' end
        if d.kind~='saved' and d.kind~='unsaved' and d.kind~='not_creative' then return false end
        local function literal(x) return type(x)=='string' and #x>0 and #x<=4096 and not x:find('[%c]') end
        return literal(d.world) and integer(d.source_generation) and d.source_generation>0
            and integer(d.source_revision) and d.source_revision>0
            and (d.kind=='saved' and literal(d.name) or d.kind~='saved' and d.name==nil)
    end
    local function localDocument()
        local token,d=DocumentOwner.capture()
        if token and DocumentOwner.current(token) then return token,d end
    end
    local function hostDocument()
        local _,d=localDocument()
        if not d or d.role~='host' then return {kind='unavailable'} end
        return {kind=d.document_kind,world=d.world,name=d.name,source_generation=d.document_generation,source_revision=d.revision}
    end
    local function expectedDocument(p)
        if not p then return end
        local token,d=localDocument();if not token or not endpoint then return end
        if endpoint.host then
            if d.role~='host' then return end
            return hostDocument()
        end
        local fact=p.remoteDocument
        if not fact or fact.kind=='unavailable' or d.role~='client' or d.peer_session~=p.remote
            or d.world~=fact.world or d.document_kind~=fact.kind or d.name~=fact.name
            or d.source_generation~=fact.source_generation or d.source_revision~=fact.source_revision then return end
        return fact
    end
    local function docPair(d) return d and d.source_generation..':'..d.source_revision end
    local function featuresAllowed(p)
        return not Core.modpack or Core.modpack.client_features_allowed(endpoint and endpoint.host==true,p and p.remotePolicy)
    end
    local function baseCurrent(p,channel)
        return (channel=='core' or featuresAllowed(p)) and p and not p.capacity and not p.withdrawn and endpointCurrent(endpoint) and p.compatible==true and p.remote~=nil
    end
    local function row(p,channel)
        return {channel=channel,pc_key=p.key,display_name='Игрок',epoch=L.epoch,host=endpoint and endpoint.host or false,
            local_session=endpoint and endpoint.nonce,local_revision=endpoint and endpoint.revision,
            remote_session=p.remote,remote_revision=p.remoteRevision,status=p.status or 'pending',
            compatible=baseCurrent(p,channel)==true,packages_compatible=p.compatible==true,
            fact_kind=p.withdrawn and 'explicit_core_withdrawal' or 'last_acknowledged_packages',
            withdrawal_revision=p.withdrawalRevision,problems=api.json.array({table.unpack(p.problems or {})})}
    end
    function S.snapshot()
        if closed or not api.is_live() then return nil end
        local rows={};local ids={};for key in pairs(peers) do ids[#ids+1]=key end;table.sort(ids)
        for _,key in ipairs(ids) do rows[#rows+1]=row(peers[key]) end
        return {revision=revision,epoch=L.epoch,ready=endpointCurrent(endpoint)==true and featuresAllowed(endpoint and peers[endpoint.key]),
            local_session=endpoint and endpoint.nonce,local_revision=endpoint and endpoint.revision,
            host=endpoint and endpoint.host or localOwner and localOwner.host or false,
            pc_key=localOwner and localOwner.key,overflow=overflow,peers=api.json.array(rows)}
    end
    publish=function(reason)
        revision=revision+1;local currentRevision=revision;local list={table.unpack(observers)}
        local firstError,failed
        for _,o in ipairs(list) do
            if revision~=currentRevision or closed or not api.is_live() then break end
            if o.alive and ownerLive(o.owner) then
                o.revision=currentRevision
                local ok,err=pcall(o.fn,S.snapshot(),reason)
                if not ok and not failed then firstError=err;failed=true end
            end
        end
        if failed then error(firstError,0) end
    end
    function S.observe(owner,fn)
        if closed or not api.is_live() or not ownerLive(owner) then return nil end
        assert(type(fn)=='function','readiness observer must be a function')
        local o={owner=owner,fn=fn,alive=true};observers[#observers+1]=o
        local function cancel()
            if not o.alive then return end;o.alive=false;o.fn=nil;o.owner=nil
            for i=#observers,1,-1 do if observers[i]==o then table.remove(observers,i) end end
        end
        local owned,err=pcall(owner.own,cancel,cancel)
        if not owned then cancel();error(err,0) end
        if not ownerLive(owner) or closed or not api.is_live() then cancel();return nil end
        if o.alive and ownerLive(owner) and not closed and api.is_live() then
            o.revision=revision;local ok,err=pcall(o.fn,S.snapshot(),'replay')
            if not ok then cancel();error(err,0) end
        end
        return cancel
    end
    local function peerKey(pc)
        if type(pc)=='string' then return pc end
        if not live(L.epoch) then return end
        local G=nativeWalk(L.epoch);local actor=G.unwrap(pc);return G.key(actor)
    end
    function S.current(view)
        if type(view)~='table' or not endpointCurrent(endpoint) or view.epoch~=endpoint.epoch then return false end
        local p=peers[view.pc_key]
        return baseCurrent(p,view.channel) and p.remote==view.remote_session and p.remoteRevision==view.remote_revision
            and endpoint.nonce==view.local_session and endpoint.revision==view.local_revision or false
    end
    function S.peer(pc,channel)
        if not endpointCurrent(endpoint) then return end
        local epoch=L.epoch;local key=peerKey(pc)
        if not live(epoch) then return nil end
        local p=key and peers[key];local view=p and row(p,channel)
        return view and S.current(view) and view or nil
    end
    function S.compatible(pc) return S.peer(pc)~=nil end
    function S.accept(pc,source,target,sourceRevision,targetRevision,channel)
        if not endpointCurrent(endpoint) then return end
        local epoch=L.epoch;local key=peerKey(pc)
        if not live(epoch) then return end
        local p=key and peers[key];if not p then return end
        local view=row(p,channel);view.direction='receive'
        return S.current(view) and view.remote_session==source and view.local_session==target
            and view.remote_revision==sourceRevision and view.local_revision==targetRevision and view or nil
    end
    local function nativeRoute(p,e,ownerRevision,owner)
        local admission=p.admission
        local function current()
            local function held() return peers[p.key]==p and p.admission==admission and not p.withdrawn end
            return held() and endpointCurrent(e) and e.revision==ownerRevision
                and (not owner or ownerLive(owner)) and held()
        end
        local actor=p.pc
        local G=nativeWalk(e.epoch,current)
        if not G.valid(actor) or not G.current() or p.pc~=actor then return end
        local key=G.key(actor)
        if not current() or p.pc~=actor or key~=p.key then return end
        local host=G.authority(actor)
        if not G.current() or p.pc~=actor then return end
        local localPlayer=G.call(actor,'IsLocalPlayerController')
        if not current() or p.pc~=actor then return end
        if e.host and (host~=true or localPlayer==true) or not e.host and (host==true or localPlayer~=true) then return end
        return actor,G
    end
    function S.send(owner,pc,text,view)
        -- Core scenery must reach matching modded peers even when vanilla guests
        -- are allowed. Gameplay feature traffic still follows the host policy.
        if view and view.channel=='core' and (type(owner)~='table'or owner.id~='Core') then return false,'Core channel owner required' end
        if not ownerLive(owner) or type(view)~='table' or view.direction=='receive' or not S.current(view) then return false,'retired' end
        if type(text)~='string' or #text>900 then return false,'invalid' end
        local epoch=L.epoch;local key=peerKey(pc)
        if not ownerLive(owner) or not live(epoch) or not S.current(view) or key~=view.pc_key then return false,'retired' end
        local p,e=peers[key],endpoint;local ownerRevision=e.revision
        local actor,G=nativeRoute(p,e,ownerRevision,owner)
        if not actor or not S.current(view) then return false,'retired' end
        local mode=api.context().mode
        if not ownerLive(owner) or not S.current(view) then return false,'retired' end
        if mode=='menu' then return false,'menu' end
        local fn=G.read(actor,e.host and 'ClientSetRevisionNumber' or 'ExecuteConsoleCommandFromClient')
        if not G.current() or not ownerLive(owner) or not S.current(view) or p.pc~=actor then return false,'retired' end
        if type(fn)~='function' and type(fn)~='userdata' then return false,'invalid' end
        fn(actor,text)
        if not G.current() then return false,'retired' end
        if ownerLive(owner) and S.current(view) then return true end
        return false,'retired'
    end
    local function warn(problems)
        local epoch=L.epoch
        if #problems==0 or not api.is_live() or closed then return end
        local message='Mod versions differ:\n'..table.concat(problems,'\n')
        api.log(message)
        for _,gi in ipairs(registry.list('B_GameInstance_C')) do
            if not live(epoch) then return end
            local G=nativeWalk(epoch)
            if G.valid(gi) then
                local manager=G.read(gi,'MenuUIManager')
                if G.valid(manager) then G.call(manager,'ShowMessageDialog','Mods — update required',message) end
            end
        end
    end
    local function retireFacts(reason)
        revision=revision+1;pendingNotification=reason -- trusted retirement is pure
    end
    local function notifyRetirement()
        local reason=pendingNotification;pendingNotification=nil
        if reason and not closed and api.is_live() then publish(reason) end
    end
    local function invalidateEndpoint(reason)
        endpoint=nil;queue={};intents={};intentOrder={};dispatchKey=nil
        for _,p in pairs(peers) do
            p.compatible=false;p.status=p.withdrawn and 'withdrawn' or p.capacity and 'capacity' or 'pending';p.ack=nil;p.sent=nil;p.partial=nil;p.documentAck=nil;p.confirmSent=nil;p.confirmReceived=nil
        end
        if not closed and api.is_live() then publish(reason) end
    end
    local function sendAllowed(e,ownerRevision)
        if not endpointCurrent(e) or ownerRevision and e.revision~=ownerRevision then return false end
        local mode=api.context().mode
        return mode~='menu' and endpointCurrent(e) and (not ownerRevision or e.revision==ownerRevision)
    end
    local function advanceEndpoint(reason)
        if endpoint.revision>=MAX then invalidateEndpoint('state revision exhausted');return false end
        endpoint.revision=endpoint.revision+1;queue={};intents={};intentOrder={};dispatchKey=nil
        for _,p in pairs(peers)do
            p.compatible=false;p.status=p.withdrawn and 'withdrawn' or p.capacity and 'capacity' or 'pending'
            p.ack=nil;p.sent=nil;p.documentAck=nil;p.confirmSent=nil;p.confirmReceived=nil
        end
        publish(reason);return true
    end
    function S.policy_changed()
        if endpointCurrent(endpoint) and endpoint.host then
            if advanceEndpoint('host modpack policy changed')then reconcile()end
        else publish('local host preference changed')end
    end
    local function forgetPeer(key)
        if Core.modpack then Core.modpack.left(nil,key)end
        peers[key]=nil
    end
    local function packetCurrent(packet)
        return endpointCurrent(packet.endpoint) and packet.localRevision==packet.endpoint.revision
            and peers[packet.peer.key]==packet.peer and (packet.target=='-' or packet.peer.remote==packet.target)
            and not packet.peer.withdrawn
    end
    fillQueue=function()
        local remaining={}
        for _,intent in ipairs(intentOrder) do
            local bucket=intents[intent.peer]
            if bucket and bucket[intent.kind]==intent then
                if not packetCurrent(intent) then bucket[intent.kind]=nil
                elseif #queue+#intent.texts<=256 and ((intent.kind~='A' and intent.kind~='C') or not bucket.R and not bucket.S) then
                    bucket[intent.kind]=nil
                    for _,text in ipairs(intent.texts) do
                        queue[#queue+1]={peer=intent.peer,endpoint=intent.endpoint,localRevision=intent.localRevision,
                            kind=intent.kind,target=intent.target,token=intent.token,text=text,documentPair=intent.documentPair,remoteRevision=intent.remoteRevision}
                    end
                else remaining[#remaining+1]=intent end
                if not next(bucket) then intents[intent.peer]=nil end
            end
        end
        intentOrder=remaining
    end
    local function retireTransfer(failed)
        if failed.peer.remote==failed.target and failed.peer.remoteRevision==failed.remoteRevision
            or failed.target=='-' and failed.peer.remote==nil then
            failed.peer.sent=nil;failed.peer.ackSent=nil -- only the failed current publication may be resubmitted
        end
        local bucket=intents[failed.peer]
        if bucket then
            local intent=bucket[failed.kind]
            if intent and intent.token==failed.token and intent.endpoint==failed.endpoint and intent.localRevision==failed.localRevision then bucket[failed.kind]=nil end
            if not next(bucket) then intents[failed.peer]=nil end
        end
        local kept={}
        for _,packet in ipairs(queue) do
            if packet.peer~=failed.peer or packet.token~=failed.token or packet.endpoint~=failed.endpoint then kept[#kept+1]=packet end
        end
        queue=kept;fillQueue()
    end
    ensureSend=function()
        local e=endpoint;local ownerRevision=e and e.revision
        fillQueue()
        if dispatchKey and C.work_budget.keys[dispatchKey]~=dispatchJob then dispatchKey=nil;dispatchJob=nil end
        if not sendAllowed(e,ownerRevision) or #queue==0 or dispatchKey then return false end
        dispatchSerial=dispatchSerial+1;local key='modsync-send:'..dispatchSerial;dispatchKey=key
        local stepFn=function()
            if not sendAllowed(e,ownerRevision) then if dispatchKey==key then dispatchKey=nil end;return true end
            local packet=queue[1]
            if packet and packet.endpoint==e and packetCurrent(packet) then
                local ok,value=pcall(function()
                    local p=packet.peer;local actor,G=nativeRoute(p,e,ownerRevision)
                    if not sendAllowed(e,ownerRevision) then return 'blocked' end
                    if not actor then if queue[1]==packet then table.remove(queue,1) end;return end
                    local fn=G.read(actor,e.host and 'ClientSetRevisionNumber' or 'ExecuteConsoleCommandFromClient')
                    if not G.current() or not sendAllowed(e,ownerRevision) or not packetCurrent(packet) or p.pc~=actor then return end
                    if type(fn)~='function' and type(fn)~='userdata' then return end
                    if queue[1]~=packet then return end
                    table.remove(queue,1);fillQueue();fn(actor,packet.text)
                    if (packet.kind=='R' or packet.kind=='S') and p.loggedInventory~=packet.token then
                        p.loggedInventory=packet.token
                        api.log('WIR2 inventory sent role='..(e.host and 'host' or 'client')..' peer='..p.key..' kind='..packet.kind..' core='..Core.version)
                    end
                    if not G.current() then return end
                    if packet.kind=='C' and packetCurrent(packet) then
                        local d=expectedDocument(p)
                        if d and packet.documentPair==docPair(d) then p.confirmSent=packet.documentPair;refreshCompatible(p) end
                    end
                end)
                if not ok then
                    if sendAllowed(e,ownerRevision) then retireTransfer(packet);if dispatchKey==key then dispatchKey=nil end;pcall(ensureSend) end
                    error(value,0)
                end
                if value=='blocked' then if dispatchKey==key then dispatchKey=nil end;return true end
            elseif packet then table.remove(queue,1) end
            fillQueue()
            if not sendAllowed(e,ownerRevision) or #queue==0 then if dispatchKey==key then dispatchKey=nil end;return true end
            return false
        end
        local submitted,accepted=pcall(C.work_budget.submit,key,'package readiness',stepFn)
        if not submitted then
            local job=C.work_budget.keys[key]
            if job and job.step==stepFn then pcall(C.work_budget.cancel,key) end
            if dispatchKey==key then dispatchKey=nil end;error(accepted,0)
        end
        if accepted==false and dispatchKey==key then dispatchKey=nil end
        if dispatchKey==key then dispatchJob=C.work_budget.keys[key] end
        return accepted
    end
    local function enqueue(p,kind,target,body)
        local e=endpoint;local ownerRevision=e and e.revision
        local admission=p.admission
        local function held() return peers[p.key]==p and p.admission==admission and not p.withdrawn end
        if not held() or not endpointCurrent(e) or not held() then return false end
        local data=api.json.encode(body)
        if not held() or not endpointCurrent(e) or e.revision~=ownerRevision or not held()
            or target~='-' and p.remote~=target then return false end
        assert(#data<=12288,'package inventory exceeds 12 KiB')
        local total=math.ceil(#data/384);if total>32 then return false end
        transferSerial=transferSerial+1
        local token=transferSerial..':'..kind..':'..target..':'..ownerRevision..':'..((kind=='A' or kind=='C') and body.ack or '')..':'..(body.document_generation or '')..':'..(body.document_revision or '')
        local texts={}
        for i=1,total do texts[i]=PREFIX..kind..':'..e.nonce..':'..target..':'..ownerRevision..':'..i..':'..total..':'..hex(data:sub((i-1)*384+1,i*384)) end
        -- Queue pressure never discards accepted work. One latest intent for each
        -- route/kind waits for finite packet progress; no timer or resend producer.
        local bucket=intents[p] or {};intents[p]=bucket
        local old=bucket[kind]
        if old then for i=#intentOrder,1,-1 do if intentOrder[i]==old then table.remove(intentOrder,i) end end end
        local kept={}
        for _,packet in ipairs(queue) do
            if packet.peer~=p or packet.kind~=kind or packet.token==token and packet.localRevision==ownerRevision then kept[#kept+1]=packet end
        end
        queue=kept
        local intent={peer=p,endpoint=e,localRevision=ownerRevision,kind=kind,target=target,token=token,texts=texts,remoteRevision=p.remoteRevision,documentPair=body.document_generation and body.document_generation..':'..body.document_revision}
        bucket[kind]=intent;intentOrder[#intentOrder+1]=intent
        fillQueue();ensureSend();return true
    end
    announce=function(p,kind)
        if p.capacity or p.withdrawn then return false end
        local e=endpoint;local ownerRevision=e and e.revision
        local admission=p.admission
        local function held() return peers[p.key]==p and p.admission==admission and not p.withdrawn end
        if not held() or not endpointCurrent(e) or not held() then return false end
        local target=p.remote or '-';local signature=e.nonce..':'..ownerRevision..':'..target..':'..kind..':'..(p.remoteRevision or 0)
        if p.sent==signature then return true end
        local body={protocol=2,packages=Public.wire(inventory.packages,api.json),document=e.host and hostDocument() or {kind='unavailable'}}
        if e.host then body.require_modpack=not Core.modpack or Core.modpack.require_modpack()end
        if not held() then return false end
        local ok=enqueue(p,kind,target,body)
        if ok and held() and endpointCurrent(e) and e.revision==ownerRevision and held()
            and (target=='-' or p.remote==target) then p.sent=signature end
        return ok
    end
    refreshCompatible=function(p)
        local admission=p.admission
        local function held() return peers[p.key]==p and p.admission==admission end
        if not held() then return end
        local compatible=not p.withdrawn and endpointCurrent(endpoint) and p.remote~=nil and p.ack==endpoint.revision and #(p.problems or {})==0
        if not held() then return end
        p.compatible=compatible
        local ready=baseCurrent(p)==true
        if not held() then return end
        local status=p.withdrawn and 'withdrawn' or p.capacity and 'capacity' or compatible and 'compatible' or #(p.problems or {})>0 and 'mismatch' or 'pending'
        if p.ready~=ready or p.status~=status then p.ready=ready;p.status=status;publish('peer capability committed') end
    end
    local function addPeer(pc,key)
        local p=peers[key]
        if p then
            if p.pc~=pc then p.admission={} end
            p.pc=pc;return p
        end
        local count=0;for _ in pairs(peers) do count=count+1 end
        if count>=32 then overflow=true;return nil end
        if Core.modpack then Core.modpack.joined(pc)end
        p={pc=pc,key=key,admission={},status='pending',compatible=false,problems={},boots={},bootCount=0};peers[key]=p
        publish('native route admitted');return peers[key]==p and p or nil
    end
    local function receive(self,value,up)
        local epoch=L.epoch;if not live(epoch) or api.context().mode=='menu' then return end
        local G=nativeWalk(epoch)
        local pc=G.unwrap(self);local raw=G.unwrap(value);local text=G.text(raw)
        if not live(epoch) or type(text)~='string' or #text>900 then return end
        if not text:match('^WIR%d+:') then return end
        if not G.valid(pc) or not G.current() then return end
        local host=G.authority(pc);if not G.current() then return end
        local localPlayer=G.call(pc,'IsLocalPlayerController');if not G.current() then return end
        if up and (not host or localPlayer==true) or not up and (host or localPlayer~=true) then return end
        local key=G.key(pc);if not G.current() or type(key)~='string' or key=='' then return end
        if up and not localOwner then mountServer(pc,key,epoch) end
        if not live(epoch) then return end
        local p=addPeer(pc,key);if not p or not live(epoch) or peers[key]~=p or p.capacity then return end
        if text:sub(1,#PREFIX)~=PREFIX then
            if p.status~='unsupported' then
                p.admission={};local admission=p.admission
                p.compatible=false;p.status='unsupported';p.problems={'Unsupported legacy/foreign runtime protocol; install WIR2 group'}
                p.partial=nil;p.ack=nil
                if endpoint and not endpoint.host and p.remote then DocumentOwner.withdraw_peer(p.remote) end
                if not live(epoch) or peers[key]~=p or p.admission~=admission then return end
                publish('protocol revoked')
                if live(epoch) and peers[key]==p and p.admission==admission then warn(p.problems) end
            end
            return -- no response/amplification to unsupported frames
        end
        local kind,source,target,rev,i,total,data=text:sub(#PREFIX+1):match('^([RSACW]):([0-9a-f]+%-%d+):([0-9a-f%-]+):(%d+):(%d+):(%d+):(%x+)$')
        rev,i,total=tonumber(rev),tonumber(i),tonumber(total)
        local sourceBoot,sourceGen=session(source)
        if not kind or not sourceBoot or not integer(rev) or rev<1 or not i or i<1 or not total or total<1 or total>32 or i>total or #data>768 or #data%2~=0 then return end
        local e=endpoint;if not endpointCurrent(e) then return end
        local ownerRevision=e.revision
        -- A private identity captures this actual input admission. Reentrant W
        -- or another accepted publication supersedes it even when the same peer
        -- table/session/revision remains, or a successor clears withdrawn again.
        local admission=p.admission
        local function inputCurrent()
            local function held() return peers[key]==p and p.admission==admission end
            return held() and endpointCurrent(e) and e.revision==ownerRevision and held()
        end
        local function commitInput()
            if not inputCurrent() then return false end
            p.admission={};admission=p.admission
            return true
        end
        if target~='-' and target~=e.nonce or target=='-' and kind~='R' then return end
        if e.host and Core.modpack then Core.modpack.packet(pc)end
        if kind=='W' then
            -- Only a literal withdrawal by the already admitted exact session
            -- can retire its rights. Silence and invocation success do not.
            if source~=p.remote or not p.remoteRevision or rev<p.remoteRevision
                or total~=1 or i~=1 or p.withdrawn
                or p.partial and p.partial.source==source and p.partial.revision>rev then return end
            local ok,body=pcall(api.json.decode,unhex(data))
            if not ok or type(body)~='table' or body.protocol~=2 or body.owner~='Core'
                or not inputCurrent() or peers[key]~=p or p.remote~=source then return end
            for field in pairs(body) do if field~='protocol' and field~='owner' then return end end
            if not commitInput() then return end
            p.withdrawn=source;p.withdrawalRevision=rev;p.compatible=false;p.ready=false;p.status='withdrawn'
            p.partial=nil;p.ack=nil;p.sent=nil;p.documentAck=nil;p.confirmSent=nil;p.confirmReceived=nil
            -- The last acknowledged package revision/fingerprint remain historical.
            -- Pure revocation precedes document listeners and cannot erase reentry.
            if not e.host then DocumentOwner.withdraw_peer(source) end
            if inputCurrent() and peers[key]==p and p.remote==source and p.withdrawn==source then
                publish('explicit Core owner withdrawal')
            end
            return
        end
        if p.withdrawn==source then return end -- a retired incarnation cannot resurrect
        local previous=p.boots[sourceBoot]
        if previous and sourceGen<previous or previous and sourceGen==previous and p.remote~=source then return end
        if (kind=='A' or kind=='C') and (source~=p.remote or rev~=p.remoteRevision or total~=1) then return end
        if source==p.remote and p.remoteRevision and rev<p.remoteRevision then return end
        local signature=kind..':'..source..':'..target..':'..rev
        local t=(kind=='R' or kind=='S') and p.partial or nil
        if t and t.source==source and rev<t.revision then return end
        if not t or t.signature~=signature then
            t={signature=signature,source=source,revision=rev,total=total,parts={},count=0}
            if kind=='R' or kind=='S' then p.partial=t end
        end
        if t.total~=total then if kind=='R' or kind=='S' then p.partial=nil end;return end
        if t.parts[i] then if t.parts[i]~=data and kind~='A' then p.partial=nil end;return end
        t.parts[i]=data;t.count=t.count+1;if t.count~=total then return end
        if kind=='R' or kind=='S' then p.partial=nil end;local raw=unhex(table.concat(t.parts));if #raw>12288 then return end
        local ok,body=pcall(api.json.decode,raw)
        if not ok or type(body)~='table' or body.protocol~=2 or not inputCurrent() or peers[key]~=p then return end
        local allowed=(kind=='A' or kind=='C') and {protocol=true,ack=true,document_generation=true,document_revision=true}
            or {protocol=true,packages=true,document=true,require_modpack=not e.host}
        for field in pairs(body) do if not allowed[field] then return end end
        if kind=='A' or kind=='C' then
            if not integer(body.ack) or body.ack~=e.revision then return end
            local d=expectedDocument(p)
            if not inputCurrent() then return end
            local proof=integer(body.document_generation) and integer(body.document_revision)
                and body.document_generation>0 and body.document_revision>0
                and body.document_generation..':'..body.document_revision
            if kind=='C' then
                if e.host or not d or proof~=docPair(d) then return end
                if not commitInput() then return end
                p.confirmReceived=proof;p.ack=body.ack;refreshCompatible(p);return
            end
            if not commitInput() then return end
            p.ack=body.ack
            if e.host and d and proof==docPair(d) then
                p.documentAck=proof
                if p.confirmSent~=proof then enqueue(p,'C',source,{protocol=2,ack=rev,document_generation=body.document_generation,document_revision=body.document_revision}) end
            end
            if not inputCurrent() then return end
            refreshCompatible(p);return
        end
        if not inventoryShape(body.packages) or not documentShape(body.document) or e.host and body.document.kind~='unavailable'
            or body.require_modpack~=nil and type(body.require_modpack)~='boolean' then return end
        local good,problems=pcall(Public.mismatch,inventory.packages,body.packages,e.host)
        if not good or type(problems)~='table' or not inputCurrent() or peers[key]~=p then return end
        if not commitInput() then return end
        -- Same boot has a monotonic generation. Unknown foreign boots have no
        -- global ordering claim; a completed READY explicitly replaces the old endpoint.
        if source~=p.remote then
            if kind~='R' and target~=e.nonce then return end
            if not p.boots[sourceBoot] then
                if p.bootCount>=32 then
                    if not e.host and p.remote then DocumentOwner.withdraw_peer(p.remote) end
                    if not inputCurrent() or peers[key]~=p then return end
                    p.capacity=true;p.compatible=false;p.ack=nil;p.sent=nil;p.partial=nil;p.status='capacity'
                    p.problems={'Endpoint session history capacity reached; explicit native route retirement required'}
                    overflow=true;publish('endpoint session capacity revoked');return
                end
            end
            local previousRemote=p.remote
            if not e.host and previousRemote then DocumentOwner.withdraw_peer(previousRemote) end
            if not inputCurrent() or peers[key]~=p or p.remote~=previousRemote then return end
            if not p.boots[sourceBoot] then p.bootCount=p.bootCount+1 end
            p.boots[sourceBoot]=sourceGen;p.remote=source;p.remoteRevision=nil;p.fingerprint=nil;p.ack=nil;p.sent=nil
            p.withdrawn=nil;p.withdrawalRevision=nil
            p.documentAck=nil;p.confirmSent=nil;p.confirmReceived=nil
        end
        if p.remoteRevision==rev and p.fingerprint and p.fingerprint~=raw then
            if not e.host then DocumentOwner.withdraw_peer(source) end
            if not inputCurrent() or peers[key]~=p or p.remote~=source then return end
            p.compatible=false;p.status='mismatch';p.problems={'Conflicting package inventory at one committed revision'}
            p.ack=nil;publish('protocol conflict');return
        end
        local changed=p.remoteRevision~=rev or p.fingerprint~=raw
        p.remoteRevision=rev;p.fingerprint=raw;p.problems=problems
        if changed then api.log('WIR2 inventory received role='..(e.host and 'host' or 'client')..' peer='..key..' core='..tostring(Public.map(body.packages).core))end
        if e.host and Core.modpack then Core.modpack.reply(pc,Public.map(body.packages),problems)end
        if not e.host then
            p.remotePolicy=body.require_modpack
            if changed then api.log('WIR2 host policy received: Require modpack='..tostring(p.remotePolicy))end
            p.remoteDocument=body.document
            local function publicationCurrent()
                return inputCurrent() and not p.withdrawn and peers[key]==p and p.remote==source and p.remoteRevision==rev and p.fingerprint==raw
            end
            if #problems>0 then DocumentOwner.withdraw_peer(source) else DocumentOwner.bind_peer(source,body.document,publicationCurrent) end
            if not inputCurrent() or peers[key]~=p or p.remote~=source or p.remoteRevision~=rev then return end
        end
        if changed then p.compatible=false;p.status=#problems>0 and 'mismatch' or 'pending';publish('remote inventory committed') end
        if not inputCurrent() or peers[key]~=p or p.remote~=source or p.remoteRevision~=rev then return end
        if changed and #problems>0 then
            warn(problems)
            if not e.host and Core.modpack and Core.modpack.refuse_host then
                Core.modpack.refuse_host(e.pc,Public.map(body.packages))
            end
        end
        if not inputCurrent() or peers[key]~=p or p.remote~=source or p.remoteRevision~=rev then return end
        -- One STATE per new readiness/inventory commit, not per duplicate frame.
        if kind=='R' or changed then announce(p,'S') end
        if inputCurrent() and peers[key]==p and p.remote==source and p.remoteRevision==rev then
            local d=expectedDocument(p)
            if not inputCurrent() then return end
            local ackSignature=e.nonce..':'..source..':'..rev..':'..e.revision..':'..(docPair(d) or '')
            if p.ackSent~=ackSignature then
                local accepted=enqueue(p,'A',source,{protocol=2,ack=rev,document_generation=d and d.source_generation,document_revision=d and d.source_revision})
                if accepted and inputCurrent() and peers[key]==p then p.ackSent=ackSignature end
            end
            if not inputCurrent() then return end
            refreshCompatible(p)
        end
    end
    local function normalized(key)
        if type(key)~='string' then return end
        local address,name=key:match('^([^|]+)|(.+)$');return address and address..':'..name or key
    end
    local function mountLocal(record)
        local epoch=L.epoch;local runtime=WIR_RUNTIME;local current=runtime and runtime.input_owner
        if not live(epoch) or type(record)~='table' or type(current)~='table'
            or record.key~=current.key or record.world~=current.world or record.generation~=current.generation
            or runtime.world_generation~=record.generation or runtime.loading or runtime.state_retired then return end
        local pc=runtime.input and runtime.input.controller
        local G=nativeWalk(epoch,function() return runtime==WIR_RUNTIME and runtime.input_owner==current and runtime.input and runtime.input.controller==pc end)
        if not G.valid(pc) then return end
        local key=G.key(pc)
        if not live(epoch) or key~=normalized(record.key) or normalized(record.world)~=L.world then return end
        local host=G.authority(pc)
        if not G.current() or runtime.input_owner~=current then return end
        local nextOwner={pc=pc,key=key,world=record.world,generation=record.generation,host=host==true}
        if localOwner and localOwner.key==key and localOwner.world==record.world and localOwner.generation==record.generation then localOwner.pc=pc;return end
        localOwner=nextOwner;invalidateEndpoint('local endpoint replaced');reconcile()
    end
    mountServer=function(pc,key,epoch)
        if localOwner or not live(epoch) or not L.world then return end
        -- An actual authoritative remote controller route proves server role.
        -- It does not prove the remote Lua endpoint; that remains pending.
        local G=nativeWalk(epoch,function()return localOwner==nil end)
        local world=G.worldKey(pc)
        if not G.current() or world~=L.world then return end
        localOwner={key='server:'..world,world=world,generation=epoch,host=true}
        registry.ready(pc);reconcile()
    end
    reconcile=function()
        local epoch=L.epoch
        if not localOwner and live(epoch) and L.world then
            for _,pc in ipairs(registry.list('B_PlayerController_C')) do
                if not live(epoch) then return end
                local G=nativeWalk(epoch,function()return localOwner==nil end)
                if G.valid(pc) and G.authority(pc)==true and G.current() then
                    local isLocal=G.call(pc,'IsLocalPlayerController')
                    if not G.current() then return end
                    if isLocal~=true then
                        local world=G.worldKey(pc)
                        if not G.current() then return end
                        if world==L.world then localOwner={key='server:'..world,world=world,generation=epoch,host=true};break end
                    end
                end
            end
        end
        if not live(epoch) or not localOwner then return end
        local owner=localOwner;local seen={};overflow=false
        local function currentOwner() return live(epoch) and localOwner==owner end
        if owner.host then
            for _,pc in ipairs(registry.list('B_PlayerController_C')) do
                if not currentOwner() then return end
                local G=nativeWalk(epoch,currentOwner)
                if G.valid(pc) and G.current() then
                    local authority=G.authority(pc)
                    if not G.current() then return end
                    local isLocal=G.call(pc,'IsLocalPlayerController')
                    if not G.current() then return end
                    if authority==true and isLocal~=true then
                        local key=G.key(pc);if not G.current() then return end
                        if type(key)=='string' and key~='' then seen[key]=true;addPeer(pc,key) end
                    end
                end
            end
        else seen[owner.key]=true;addPeer(owner.pc,owner.key) end
        if not currentOwner() then return end
        local removed=false
        for key,p in pairs(peers) do
            if not seen[key] then forgetPeer(key);removed=true
            elseif p.status=='capacity' then overflow=true end
        end
        if removed then publish('native routes retired') end
        if not currentOwner() or not hookProof or not hookProof.ready or not hookProof.healthy or not inventory
            or not inventoryOwnsCore() or not api.packages.owner_current() or not api.packages.is_current(inventory.revision) then return end
        local mode=api.context().mode;if not currentOwner() or mode=='menu' then return end
        if not endpoint then
            local generation=api.allocate_owner_generation('Core.modsync')
            if not currentOwner() or not integer(generation) or generation<1 then return end
            endpoint={epoch=epoch,key=localOwner.key,pc=localOwner.pc,host=localOwner.host,nonce=boot..'-'..generation,revision=1,inventoryRevision=inventory.revision}
            api.log('WIR2 endpoint role='..(endpoint.host and 'host' or 'client')..' peer='..endpoint.key..' core='..Core.version..' world='..tostring(L.world))
            lastDocument=api.json.encode(endpoint.host and hostDocument() or {kind='unavailable'})
            publish('endpoint committed')
        end
        local e=endpoint;if not endpointCurrent(e) then return end
        for _,p in pairs(peers) do if endpointCurrent(e) then announce(p,p.remote and 'S' or 'R') end end
        ensureSend()
    end
    -- Receive subscriptions exist before any admission/replay can announce.
    api.hook(PATHS[1],function(self,value) receive(self,value,true) end,{finite_install=true})
    api.hook(PATHS[2],function(self,value) receive(self,value,false) end,{finite_install=true})
    api.hooks.observe(PATHS,function(proof)
        if not api.is_live() or closed then return end
        local current=api.hooks.snapshot(PATHS)
        if not current or current.revision~=proof.revision or current.ready~=proof.ready then return end
        hookProof=proof
        if not proof.ready then invalidateEndpoint('RPC endpoint unavailable') else reconcile() end
    end)
    api.packages.observe(function(snapshot)
        local current=api.packages.snapshot()
        if not current or not snapshot or current.revision~=snapshot.revision or current.valid~=snapshot.valid then return end
        local old=inventory;inventory=snapshot
        if not snapshot or not snapshot.valid or not integer(snapshot.revision) then invalidateEndpoint('inventory revoked');return end
        if endpoint and old and old.revision~=snapshot.revision then
            endpoint.inventoryRevision=snapshot.revision
            if not advanceEndpoint('local inventory committed')then return end
        end
        reconcile()
    end)
    api.map_document.on_change(function()
        if not api.is_live() or closed then return end
        local e=endpoint
        if e and e.host then
            local signature=api.json.encode(hostDocument())
            if not endpointCurrent(e) then return end
            if lastDocument~=signature then
                lastDocument=signature
                if advanceEndpoint('authoritative document committed')then reconcile()end
            end
        else
            for _,p in pairs(peers) do refreshCompatible(p) end
        end
    end,{replay=true})
    api.on('input_owner_admitted',mountLocal,{ungated=true})
    registry.onCommitted(function() reconcile() end)
    api.on('begin_play_post',function(p)
        local epoch=L.epoch;if not live(epoch) then return end
        api.hooks.refresh(PATHS)
        local G=nativeWalk(epoch);local actor=G.unwrap(p)
        if G.valid(actor) then
            api.admit_input_owner(actor);if G.current() then registry.ready(actor) end
        end
        local runtime=WIR_RUNTIME;mountLocal(runtime and runtime.input_owner)
    end,{ungated=true,class_filter=function(name) return name:find('PlayerController',1,true)~=nil end})
    local function start()
        if complete or not live(L.epoch) or not L.world then return end
        if bootstrapKey and C.work_budget.keys[bootstrapKey]==bootstrapJob then return end
        local epoch=L.epoch;bootstrapSerial=bootstrapSerial+1
        local key='package-bootstrap:'..bootstrapSerial;bootstrapKey=key
        local steps={}
        for _,class in ipairs({'B_PlayerController_C','B_GameInstance_C'}) do
            local snapshot
            steps[#steps+1]=C.work_budget.native(function() if live(epoch) then snapshot=api.find_all(class) end end)
            steps[#steps+1]=C.work_budget.defer(function() return C.work_budget.each(snapshot or {},function(o)
                if live(epoch) then api.admit_input_owner(o);if live(epoch) then registry.ready(o) end end
            end) end)
        end
        steps[#steps+1]=function() if live(epoch) then complete=true end;if bootstrapKey==key then bootstrapKey=nil end;reconcile();return true end
        local step=C.work_budget.sequence(steps)
        local stepFn=function()
            if not live(epoch) then if bootstrapKey==key then bootstrapKey=nil end;return true end
            return step()
        end
        local ok,accepted=pcall(C.work_budget.submit,key,'finite native endpoint bootstrap',stepFn)
        if not ok then
            local job=C.work_budget.keys[key]
            if job and job.step==stepFn then pcall(C.work_budget.cancel,key) end
            if bootstrapKey==key then bootstrapKey=nil end;error(accepted,0)
        end
        if accepted==false and bootstrapKey==key then bootstrapKey=nil end
        if bootstrapKey==key then bootstrapJob=C.work_budget.keys[key] end
    end
    L.onReset(function()
        if Core.modpack then Core.modpack.retire_peers()end
        localOwner=nil;endpoint=nil;inventory=nil;peers={};queue={};intents={};intentOrder={};overflow=false
        dispatchKey=nil;complete=false;bootstrapKey=nil
        retireFacts('world retired')
    end)
    L.onRetired(function()
        if Core.modpack then Core.modpack.retire_peers(L.retired)end
        local changed=false
        for key in pairs(peers) do if L.retired[key] then forgetPeer(key);changed=true end end
        if localOwner and L.retired[localOwner.key] then localOwner=nil;endpoint=nil;queue={};intents={};intentOrder={};dispatchKey=nil;changed=true end
        if changed then retireFacts('native owner retired') end
    end)
    for _,event in ipairs({'load_map_pre','world_changed','end_play_pre'}) do
        api.on(event,notifyRetirement,{ungated=true})
    end
    api.on('world_ready',function()
        notifyRetirement();inventory=api.packages.snapshot();api.hooks.refresh(PATHS);local runtime=WIR_RUNTIME;mountLocal(runtime and runtime.input_owner);start();reconcile()
    end,{ungated=true})
    api.on('context_changed',function() reconcile();ensureSend() end,{ungated=true})
    local withdrawalBody=hex(api.json.encode({protocol=2,owner='Core'}))
    local cancelWithdrawal=api.retirement.publish(function(ticket)
        -- WIR has already set p.active=false. Ordinary API, feature views,
        -- queue/hook dispatch and document admission are quiescent. The ticket
        -- admits only this synchronous WIR-owned publication before rollback.
        local e,owner=endpoint,localOwner
        local result={routes=0,invoked=0,refused=0}
        if not e or not owner then return result end
        local ownerRevision=e.revision
        local function held()
            return ticket.is_live()==true and not closed and endpoint==e and localOwner==owner
                and e.revision==ownerRevision and L.epoch==e.epoch and not L.transition
                and hookProof and hookProof.ready==true and hookProof.healthy==true
        end
        local ids={};for key,p in pairs(peers) do if p.remote and p.remoteRevision and not p.withdrawn then ids[#ids+1]=key end end
        table.sort(ids);result.routes=#ids
        for _,key in ipairs(ids) do
            local p=peers[key]
            local actor,remote=p and p.pc,p and p.remote
            local remoteRevision=p and p.remoteRevision
            local admission=p and p.admission
            local function routeCurrent()
                return p~=nil and held() and peers[key]==p and p.pc==actor and p.remote==remote
                    and p.admission==admission and p.remoteRevision==remoteRevision and not p.withdrawn
            end
            local G=Core.guarded_native.new(ticket,L,routeCurrent)
            local admitted=G.valid(actor) and G.key(actor)==key and G.current()
            local host=admitted and G.authority(actor)
            local localPlayer=admitted and G.call(actor,'IsLocalPlayerController')
            admitted=admitted and G.current() and (e.host and host==true and localPlayer~=true
                or not e.host and host~=true and localPlayer==true)
            local fn=admitted and G.read(actor,e.host and 'ClientSetRevisionNumber' or 'ExecuteConsoleCommandFromClient')
            if admitted and G.current() and (type(fn)=='function' or type(fn)=='userdata') then
                fn(actor,PREFIX..'W:'..e.nonce..':'..remote..':'..ownerRevision..':1:1:'..withdrawalBody)
                result.invoked=result.invoked+1 -- invocation only, never delivery/remote ACK
            else result.refused=result.refused+1 end
        end
        return result
    end)
    api.own(cancelWithdrawal,cancelWithdrawal)
    local function close()
        if Core.modpack then Core.modpack.retire_peers()end
        for _,o in ipairs(observers) do o.alive=false;o.fn=nil;o.owner=nil end
        closed=true;endpoint=nil;localOwner=nil;inventory=nil;hookProof=nil;peers={};queue={};intents={};intentOrder={};dispatchKey=nil;observers={}
        return true -- pure teardown: no native reads, callbacks or formatting
    end
    api.own(close,close)
    api.hooks.refresh(PATHS)
    local runtime=WIR_RUNTIME;mountLocal(runtime and runtime.input_owner);start();reconcile()
    return S
end
return M
