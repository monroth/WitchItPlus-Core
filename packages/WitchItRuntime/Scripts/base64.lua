-- Standard base64 with padding. decode() accepts only the canonical encoding.
local B={}
local alphabet='ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/'
local digit={}
for i=1,64 do digit[alphabet:byte(i)]=i-1 end
local function char(n) return alphabet:sub(n+1,n+1) end
function B.encode(bytes)
    local out={}
    for i=1,#bytes,3 do
        local a,b,c=bytes:byte(i,i+2)
        local n=(a<<16)|((b or 0)<<8)|(c or 0)
        out[#out+1]=char(n>>18)..char((n>>12)&63)..(b and char((n>>6)&63) or '=')..(c and char(n&63) or '=')
    end
    return table.concat(out)
end
function B.decode(text)
    assert(#text%4==0 and text:match('^[A-Za-z0-9+/]*=?=?$'),'Invalid base64')
    local out={}
    for i=1,#text,4 do
        local n,pad=0,0
        for j=i,i+3 do
            local c=text:byte(j)
            if c==61 then pad=pad+1;n=n<<6 else n=(n<<6)|digit[c] end
        end
        out[#out+1]=string.char(n>>16,(n>>8)&255,n&255):sub(1,3-pad)
    end
    local bytes=table.concat(out)
    assert(B.encode(bytes)==text,'Invalid base64')
    return bytes
end
return B
