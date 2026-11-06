-- Mod data between the host and its guests, by topic. The host publishes values to
-- every guest; a guest sends requests to the host. A message is one JSON value,
-- base64 encoded and cut into chunks, one native RPC per chunk:
-- ClientSetRevisionNumber to a guest, ExecuteConsoleCommandFromClient to the host.
-- A short message goes out at once. A long one goes out in bursts paced to a share
-- of the connection's rate, so it never floods the reliable channel the game uses;
-- short messages pass it meanwhile. A newer value of a topic replaces an older one
-- still on its way.
return function(R,C)
    local N,T={},{}
    local PC='/Game/Blueprints/Player/B_PlayerController.B_PlayerController_C:'
    local PREFIX='WIR4:'
    N.LIMIT=4194304   -- largest JSON value of one message
    local CHUNK=700   -- base64 characters per RPC; with the header a chunk stays under 800
    local MAX_CHUNKS=math.ceil(math.ceil(N.LIMIT/3)*4/CHUNK)
    local BURST=12    -- chunks sent together, about 8 KB
    local SHARE=.75   -- of the connection's rate a long message may use
    local latest,watchers,requests,peers={},{},{},{}
    local incoming={} -- [controller address][topic] = message being assembled
    local outgoing={} -- [controller address] = {pc=, messages={}, cancel=}
    local serial=0

    local function deliver(pc,down,text)
        if down then pc:ClientSetRevisionNumber(text) else pc:ExecuteConsoleCommandFromClient(text) end
    end
    local drain
    local function send(pc,down,topic,value)
        serial=serial+1
        local body=R.base64.encode(R.json.encode(value,N.LIMIT))
        local count=math.ceil(#body/CHUNK)
        local chunks={}
        for i=1,count do
            chunks[i]=PREFIX..serial..':'..i..':'..count..':'..topic..':'..body:sub((i-1)*CHUNK+1,i*CHUNK)
        end
        local key=pc:GetAddress()
        local queue=outgoing[key]
        if queue then
            for i,message in ipairs(queue.messages) do
                if message.topic==topic then table.remove(queue.messages,i);break end
            end
        end
        if count<=BURST then
            for _,text in ipairs(chunks) do deliver(pc,down,text) end
            return
        end
        if not queue then queue={pc=pc,messages={}};outgoing[key]=queue end
        queue.messages[#queue.messages+1]={topic=topic,down=down,chunks=chunks,next=1}
        if not queue.cancel then drain(key) end
    end
    drain=function(key)
        local queue=outgoing[key]
        queue.cancel=nil
        for _=1,BURST do
            local message=queue.messages[1]
            if not message then break end
            deliver(queue.pc,message.down,message.chunks[message.next])
            message.next=message.next+1
            if message.next>#message.chunks then table.remove(queue.messages,1) end
        end
        if not queue.messages[1] then outgoing[key]=nil;return end
        local rate=queue.pc.NetConnection.CurrentNetSpeed*SHARE
        queue.cancel=R.after(math.ceil(BURST*CHUNK*1000/rate),function() drain(key) end)
    end

    local function notify(topic,value,pc)
        latest[topic]=value
        for _,fn in ipairs(watchers[topic] or {}) do
            if latest[topic]~=value then return end
            fn(value,pc)
        end
    end
    function N.watch(topic,fn)
        watchers[topic]=watchers[topic] or {};table.insert(watchers[topic],fn)
        if latest[topic]~=nil then fn(latest[topic]) end
    end
    function N.on_request(topic,fn) requests[topic]=fn end
    function N.publish(topic,value)
        notify(topic,value)
        for _,pc in pairs(peers) do
            if latest[topic]~=value then return end
            send(pc,true,topic,value)
        end
    end
    function N.request(topic,value)
        local s=C.session()
        if s.role=='host' then
            if requests[topic] then requests[topic](s.pc and s.pc.PlayerState,value) end
        elseif s.pc and not s.is_vanilla_session then
            send(s.pc,false,topic,value)
        end
    end
    -- Core's connection handling: the guest's per-world inventory, and the host's peers.
    function T.request(pc,topic,value) send(pc,false,topic,value) end
    function T.add(pc) peers[pc:GetAddress()]=pc end
    function T.remove(pc)
        local key=pc:GetAddress()
        local queue=outgoing[key]
        if queue and queue.cancel then queue.cancel() end
        peers[key],incoming[key],outgoing[key]=nil,nil,nil
    end
    function T.replay(pc)
        for topic,value in pairs(latest) do send(pc,true,topic,value) end
    end
    function T.send(pc,topic,value) send(pc,true,topic,value) end

    local function receive(pc,text,down)
        text=type(text)=='string' and text or text:ToString()
        if text:sub(1,#PREFIX)~=PREFIX or #text>800 then return end
        if down then
            if pc:HasAuthority() or not pc:IsLocalPlayerController() then return end
        elseif not pc:HasAuthority() or pc:IsLocalPlayerController() then return end
        local id,index,count,topic,payload=text:match('^WIR4:(%d+):(%d+):(%d+):([%w._]+):([%w+/=]*)$')
        id,index,count=tonumber(id),tonumber(index),tonumber(count)
        if not id or count>MAX_CHUNKS or index<1 or index>count then return end
        -- The host assembles only requests it answers.
        if not down and not requests[topic] then return end
        local key=pc:GetAddress()
        local open=incoming[key] or {};incoming[key]=open
        local message=open[topic]
        if index==1 then message={id=id,count=count,parts={}};open[topic]=message
        elseif not message or message.id~=id or message.count~=count or #message.parts~=index-1 then
            open[topic]=nil;return
        end
        message.parts[index]=payload
        if index<count then return end
        open[topic]=nil
        local ok,value=pcall(function()
            return R.json.decode(R.base64.decode(table.concat(message.parts)),N.LIMIT)
        end)
        if not ok then R.log('Core','Ignored malformed mod data: %s',topic);return end
        if down then notify(topic,value,pc) else requests[topic](pc.PlayerState,value) end
    end
    R.post(PC..'ClientSetRevisionNumber',function(pc,text) receive(pc,text,true) end)
    R.post(PC..'ExecuteConsoleCommandFromClient',function(pc,text) receive(pc,text,false) end)
    -- Pending bursts are world timers; the runtime cancels them with the world.
    R.on('world_end',function() latest,peers,incoming,outgoing={},{},{},{} end)
    return N,T
end
