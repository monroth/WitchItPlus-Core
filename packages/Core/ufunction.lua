local module={}
function module.call(object,method,count,...)
    assert(type(count)=='number' and count>=0 and count%1==0,'invalid UFunction parameter count')
    local args=table.pack(...)
    for index=count+1,args.n do
        assert(args[index]==nil,'extra non-nil UFunction argument: '..method)
    end
    assert(args.n>=count,'missing UFunction argument: '..method)
    return object[method](object,table.unpack(args,1,count))
end
return module
