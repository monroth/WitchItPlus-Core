-- Strict data-only JSON. No load/loadfile, executable presets, or dependencies.
local J={}
local arrayMT={__json_array=true}
function J.array(t) return setmetatable(t or {},arrayMT) end
local escapes={['"']='\\"',['\\']='\\\\',['\b']='\\b',['\f']='\\f',['\n']='\\n',['\r']='\\r',['\t']='\\t'}
local function quote(s)
    return '"'..s:gsub('[%z\1-\31\\"]',function(c) return escapes[c] or string.format('\\u%04x',c:byte()) end)..'"'
end
function J.encode(v,limit)
    limit=limit or 262144;assert(type(limit)=='number' and limit>0 and limit<=1100000,'JSON limit invalid')
    local seen={}
    local function enc(x,depth)
        assert(depth<48,'JSON too deep')
        local t=type(x)
        if t=='string' then return quote(x) end
        if t=='boolean' then return tostring(x) end
        if t=='number' then assert(x==x and math.abs(x)<math.huge,'nonfinite JSON'); return string.format('%.17g',x) end
        assert(t=='table','unsupported JSON value '..t)
        assert(not seen[x],'cyclic JSON'); seen[x]=true
        local a,out=getmetatable(x)==arrayMT,{}
        if a then
            local length=#x; local entries=0
            for k in pairs(x) do
                assert(type(k)=='number' and k%1==0 and k>=1 and k<=length,'invalid JSON array key')
                entries=entries+1
            end
            assert(entries==length,'sparse JSON array')
            for i=1,length do out[i]=enc(x[i],depth+1) end
        else
            local keys={}; for k in pairs(x) do assert(type(k)=='string','JSON object key'); keys[#keys+1]=k end
            table.sort(keys); for _,k in ipairs(keys) do out[#out+1]=quote(k)..':'..enc(x[k],depth+1) end
        end
        seen[x]=nil
        return (a and '[' or '{')..table.concat(out,',')..(a and ']' or '}')
    end
    local text=enc(v,0)..'\n'; assert(#text<=limit,'JSON size limit'); return text
end
function J.decode(s,limit)
    limit=limit or 262144;assert(type(limit)=='number' and limit>0 and limit<=1100000,'JSON limit invalid')
    assert(type(s)=='string' and #s<=limit,'JSON size limit')
    local i,n=1,#s
    local function ws() local _,b=s:find('^[ \t\r\n]*',i); i=(b or i-1)+1 end
    local function str()
        assert(s:sub(i,i)=='"','expected string'); i=i+1; local out={}
        while i<=n do
            local c=s:sub(i,i); i=i+1
            if c=='"' then return table.concat(out) end
            assert(c:byte()>=32,'control in JSON string')
            if c=='\\' then
                local e=s:sub(i,i); i=i+1
                local map={['"']='"',['\\']='\\',['/']='/',b='\b',f='\f',n='\n',r='\r',t='\t'}
                if e=='u' then
                    local h=s:sub(i,i+3); assert(h:match('^%x%x%x%x$'),'bad unicode'); i=i+4; local u=tonumber(h,16)
                    if u>=0xd800 and u<=0xdbff then
                        assert(s:sub(i,i+1)=='\\u','missing surrogate'); i=i+2
                        h=s:sub(i,i+3); assert(h:match('^%x%x%x%x$'),'bad unicode'); i=i+4
                        local low=tonumber(h,16); assert(low>=0xdc00 and low<=0xdfff,'bad surrogate')
                        u=0x10000+(u-0xd800)*1024+low-0xdc00
                    else assert(u<0xdc00 or u>0xdfff,'orphan surrogate') end
                    c=utf8.char(u)
                else c=assert(map[e],'bad escape') end
            end
            out[#out+1]=c
        end
        error('unterminated string')
    end
    local parse
    parse=function(depth)
        assert(depth<48,'JSON too deep'); ws(); local c=s:sub(i,i)
        if c=='"' then return str() end
        if c=='{' or c=='[' then
            i=i+1; local a=c=='['; local t=a and J.array() or {}; ws()
            local close=a and ']' or '}'
            if s:sub(i,i)==close then i=i+1; return t end
            while true do
                ws(); local k=a and #t+1 or str(); ws()
                if not a then assert(t[k]==nil,'duplicate JSON key'); assert(s:sub(i,i)==':','expected colon'); i=i+1 end
                t[k]=parse(depth+1); ws(); c=s:sub(i,i); i=i+1
                if c==close then return t end
                assert(c==',','expected comma')
            end
        end
        for _,v in ipairs({'true','false'}) do if s:sub(i,i+#v-1)==v then i=i+#v; return v=='true' end end
        -- null is intentionally unsupported: optional fields must be omitted.
        local token=s:sub(i):match('^-?%d+%.?%d*[eE]?[+-]?%d*')
        assert(token and token~='','invalid JSON value')
        assert(not token:match('^-?0%d') and not token:match('%.$') and not token:match('%.[eE]') and not token:match('[eE][+-]?$'),'invalid number')
        local value=tonumber(token); assert(value and value==value and math.abs(value)<math.huge,'invalid number')
        i=i+#token; return value
    end
    local v=parse(0); ws(); assert(i>n,'trailing JSON data'); return v
end
return J
