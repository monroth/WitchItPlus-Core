-- Published service leases. Revocation releases callbacks without invoking them.
local M={}
local pack,unpack=table.pack,table.unpack
local metatableOf=assert(debug.getmetatable,'Lua metatable inspection required')
function M.new(healthy)
    local publications={}
    local function revoke(record)
        if not record or not record.alive then return end
        record.alive=false
        for _,holder in pairs(record.tables) do holder.value=nil end
        for _,holder in pairs(record.functions) do holder.fn=nil end
        for cursor in pairs(record.cursors) do cursor.key=nil;cursor.iterator=nil;cursor.state=nil end
        record.tables={};record.functions={};record.proxies={};record.callables={};record.cursors={}
    end
    local function publication(value)
        -- Ephemeron caches preserve borrowed identity without keeping completed
        -- factory results alive solely because the provider remains published.
        local function cache() return setmetatable({},{__mode='k'}) end
        local record={alive=true,tables=cache(),functions=cache(),proxies=cache(),callables=cache(),cursors=cache()}
        local function current()
            assert(healthy() and record.alive,'shared service publication is inactive')
        end
        -- Every mutable service container is leased before it can acquire a
        -- callback. Only an explicitly marked, entirely valid JSON array is
        -- a data transfer, with its marker/identity preserved for JSON encode.
        local function json_data(value,seen,depth)
            local kind=type(value)
            if kind=='string' or kind=='boolean' then return true end
            if kind=='number' then return value==value and math.abs(value)<math.huge end
            if kind~='table' or depth>=48 or seen[value] then return false end
            local meta=metatableOf(value);local array=false
            if meta~=nil then
                if type(meta)~='table' or rawget(meta,'__json_array')~=true then return false end
                for key in next,meta do if key~='__json_array' then return false end end
                array=true
            end
            seen[value]=true
            local length=array and #value or 0;local count=0
            for key,item in next,value do
                if array then
                    if type(key)~='number' or key%1~=0 or key<1 or key>length then seen[value]=nil;return false end
                elseif type(key)~='string' then seen[value]=nil;return false end
                if not json_data(item,seen,depth+1) then seen[value]=nil;return false end
                count=count+1
            end
            seen[value]=nil
            return not array or count==length
        end
        local function json_array(value)
            local meta=metatableOf(value)
            return type(meta)=='table' and rawget(meta,'__json_array')==true and json_data(value,{},0)
        end
        local wrap
        local function key_value(key)
            local holder=record.proxies[key]
            if holder then return holder.value end
            holder=record.callables[key]
            return holder and holder.fn or key
        end
        local function arguments(...)
            local args=pack(...)
            for i=1,args.n do
                local holder=record.proxies[args[i]] or record.callables[args[i]]
                if holder then args[i]=holder.value or holder.fn end
            end
            return args
        end
        wrap=function(v)
            current() -- never create new captures after an owner reentrant revoke
            if record.proxies[v] or record.callables[v] then return v end
            if type(v)=='function' then
                local holder=record.functions[v]
                if not holder then
                    holder={fn=v};record.functions[v]=holder
                    holder.view=function(...)
                        current();local args=arguments(...)
                        local result=pack(holder.fn(unpack(args,1,args.n)))
                        current()
                        for i=1,result.n do result[i]=wrap(result[i]) end
                        return unpack(result,1,result.n)
                    end
                end
                record.callables[holder.view]=holder
                return holder.view
            end
            if type(v)~='table' then return v end
            local holder=record.tables[v]
            if holder then return holder.view end
            if json_array(v) then return v end
            local meta=metatableOf(v)
            holder={value=v,equality=type(meta)=='table' and rawget(meta,'__eq')~=nil};record.tables[v]=holder
            local proxy={};holder.view=proxy;record.proxies[proxy]=holder
            return setmetatable(proxy,{
                __index=function(_,key) current();return wrap(holder.value[key_value(key)]) end,
                __newindex=function(_,key,x) current();holder.value[key_value(key)]=key_value(x);current() end,
                __len=function() current();local size=#holder.value;current();return size end,
                __pairs=function()
                    current();local iterator,state,key=pairs(holder.value);current()
                    local cursor={iterator=iterator,state=state,key=key};record.cursors[cursor]=true
                    return function()
                        current();local result=pack(cursor.iterator(cursor.state,cursor.key));current()
                        cursor.key=result[1]
                        if cursor.key~=nil then
                            for i=1,result.n do result[i]=wrap(result[i]) end
                            return unpack(result,1,result.n)
                        end
                        cursor.iterator=nil;cursor.state=nil;cursor.key=nil
                        record.cursors[cursor]=nil
                    end
                end,
                __call=function(_,...)
                    current();local args=arguments(...);local result=pack(holder.value(unpack(args,1,args.n)))
                    current()
                    for i=1,result.n do result[i]=wrap(result[i]) end
                    return unpack(result,1,result.n)
                end,
                __tostring=function() current();local value=tostring(holder.value);current();return value end,
                __eq=function(a,b)
                    -- Ordinary lease identity comparisons do not invoke an owner.
                    -- Delegate only an actual source equality metamethod.
                    local value=holder.value;local meta=value and metatableOf(value)
                    if not value and holder.equality then current() end
                    if type(meta)~='table' or rawget(meta,'__eq')==nil then return rawequal(a,b) end
                    holder.equality=true;current();local equal=key_value(a)==key_value(b);current();return equal
                end,
                __lt=function(a,b) current();local value=key_value(a)<key_value(b);current();return value end,
                __le=function(a,b) current();local value=key_value(a)<=key_value(b);current();return value end,
                __add=function(a,b) current();return wrap(key_value(a)+key_value(b)) end,
                __sub=function(a,b) current();return wrap(key_value(a)-key_value(b)) end,
                __mul=function(a,b) current();return wrap(key_value(a)*key_value(b)) end,
                __div=function(a,b) current();return wrap(key_value(a)/key_value(b)) end,
                __idiv=function(a,b) current();return wrap(key_value(a)//key_value(b)) end,
                __mod=function(a,b) current();return wrap(key_value(a)%key_value(b)) end,
                __pow=function(a,b) current();return wrap(key_value(a)^key_value(b)) end,
                __unm=function(a) current();return wrap(-key_value(a)) end,
                __band=function(a,b) current();return wrap(key_value(a)&key_value(b)) end,
                __bor=function(a,b) current();return wrap(key_value(a)|key_value(b)) end,
                __bxor=function(a,b) current();return wrap(key_value(a)~key_value(b)) end,
                __bnot=function(a) current();return wrap(~key_value(a)) end,
                __shl=function(a,b) current();return wrap(key_value(a)<<key_value(b)) end,
                __shr=function(a,b) current();return wrap(key_value(a)>>key_value(b)) end,
                __concat=function(a,b) current();return wrap(key_value(a)..key_value(b)) end,
                __close=function(_,err)
                    current();local meta=metatableOf(holder.value);local close=meta and rawget(meta,'__close')
                    assert(type(close)=='function','published value is not closeable')
                    close(holder.value,key_value(err));current()
                end,
                __metatable=false,
            })
        end
        return record,wrap(value)
    end
    local E={}
    function E.publish(id,value)
        assert(healthy(),'runtime native boundary is unhealthy')
        revoke(publications[id])
        local record,view=publication(value);publications[id]=record
        return view
    end
    function E.revoke(id) revoke(publications[id]);publications[id]=nil end
    function E.clear()
        for _,record in pairs(publications) do revoke(record) end
        publications={}
    end
    return E
end
return M
