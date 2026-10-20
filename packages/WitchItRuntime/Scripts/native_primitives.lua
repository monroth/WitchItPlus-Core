-- A package scope over the existing UE4SS host. No parallel reflection/marshaling.
local Values=dofile(assert(debug.getinfo(1,'S').source:match('^@(.+[\\/])'))..'callback_values.lua')
local M={}
function M.new(R,A,trackSubscription)
 local Native={}
 local worldObjects=dofile(assert(debug.getinfo(1,'S').source:match('^@(.+[\\/])'))..'world_objects.lua').new(R.native_call)
 function Native.scope(pkg)
  assert(pkg==A.id,'Native.scope requires the calling package id')
  local E,stops,closed,spawnErrors={},{},false,{}
  local function attempt(name,fn,...)
   local ok,result=pcall(function(...)
    assert(not closed,'scope closed')
    return fn(...)
   end,...)
   if ok then return result end
   local message='Native.'..name..' ['..pkg..']: '..tostring(result)
   if name:sub(1,6)~='spawn ' or not spawnErrors[message] then A.log(message) end
   if name:sub(1,6)=='spawn ' then spawnErrors[message]=true end
   return nil,result
  end
  local function valid(o)
   assert(o and o:IsValid(),'invalid object')
   return o
  end
  local unwrap=Values.unwrap
  local function own(stop)
   assert(type(stop)=='function','subscription failed')
   if trackSubscription then stop=trackSubscription(stop) end
   stops[#stops+1]=stop
   return stop
  end
  function E.find(path,context)
   return attempt('find '..tostring(path),function()
    if context then return worldObjects(path,valid(context)) end
    local class=valid(R.native_call('StaticFindObject',path))
    local objects=R.native_call('FindAllOf',path:match('%.([^%.]+)$') or path:match('%.?([^/]+)$')) or {}
    local result={}
    for _,o in ipairs(objects) do
     o=unwrap(o)
     if o:IsValid() and o:IsA(class) and not o:GetFullName():find('Default__',1,true) then result[#result+1]=o end
    end
    return result
   end)
  end
  function E.get(o,name)
   return attempt('get '..tostring(name),function()
    local v=valid(o)[name]
    local ok,t=pcall(function() return v:type() end)
    assert(not (ok and t=='TrivialObject') and not tostring(v):match('^TrivialObject'),'member unavailable')
    if ok and t=='FWeakObjectPtr' then return v:Get() end
    if ok and t=='UScriptStruct' and o:IsA(R.native_call('StaticFindObject','/Script/UMG.WidgetNavigation')) then
     local target=v.Widget:Get()
     return {rule=v.Rule,name=v.WidgetToFocus:ToString(),target=target:IsValid() and target or nil}
    end
    return v
   end)
  end
  function E.set(o,name,value)
   return attempt('set '..tostring(name),function() valid(o)[name]=value; return true end)
  end
  -- Named parameters and outputs; the SDK still owns parameter storage and calls.
  function E.call(o,name,args)
   return attempt('call '..tostring(name),function()
    valid(o); args=args or {}
    local schema=R.native_call('WitchItNativeFunctionSchema',o,name)
    local argv,outputs={},{}
    if schema then
     for i=1,schema.count do
      local key=schema[tostring(i)]
      local value=args[key]; if value==nil then value=args[i] end
      if schema['out:'..i] then value=value or {}; outputs[key]=value end
      assert(value~=nil,'missing parameter '..key)
      argv[i]=value
     end
    else
     -- SDK methods (GetClass, GetFName, IsValid, ...) have positional arguments.
     for i=1,#args do argv[i]=args[i] end
    end
    local result=o[name](o,table.unpack(argv,1,schema and schema.count or #argv))
    for key,value in pairs(outputs) do if value[key]~=nil then outputs[key]=value[key] end end
    outputs.ReturnValue=result
    if schema and schema.return_name then outputs[schema.return_name]=result end
    return outputs
   end)
  end
  function E.new(class,outer,template)
   return attempt('new '..tostring(class),function()
    if type(class)=='string' then local why;class,why=E.load_class(class,outer);assert(class,why or 'native class loader returned nil') end
    valid(class); valid(outer)
    local userWidget=valid(R.native_call('StaticFindObject','/Script/UMG.UserWidget'))
    if class:IsChildOf(userWidget) then
     assert(template==nil,'cooked widgets do not accept a template')
     local lib=valid(R.native_call('StaticFindObject','/Script/UMG.Default__WidgetBlueprintLibrary'))
     return valid(lib:Create(outer,class,valid(outer:GetOwningPlayer())))
    end
    if template then
     return valid(R.native_call('StaticConstructObject',class,outer,FName('None'),0,0,false,false,template))
    end
    return valid(R.native_call('StaticConstructObject',class,outer))
   end)
  end
  function E.on_call(path,phase,fn)
   return attempt('on_call '..tostring(path),function()
    assert(phase=='pre' or phase=='post','invalid phase')
    return own(A.hook(path,function(self,...)
     if closed then return end
     local args=table.pack(...)
     for i=1,args.n do args[i]=unwrap(args[i]) end
     attempt('callback '..path,fn,unwrap(self),args)
    end,{phase=phase,ungated=true}))
   end)
  end
  function E.on_construct(path,fn,opts)
   return attempt('on_construct '..tostring(path),function()
    local stop=assert(E.on_call(path..':Construct','post',function(o) fn(o) end),'Construct subscription failed')
    if opts and opts.existing then
     for _,o in ipairs(E.find(path) or {}) do attempt('existing Construct '..path,fn,o) end
    end
    return stop
   end)
  end
  local backend
  local function raw(op,...)
   backend=backend or R.native_call('WitchItNativePrimitives')
   assert(type(backend)=='function','native primitives unavailable')
   return backend(op,...)
  end
  local function subscribe(op,path,fn)
   local active=true
   local function invoke(...)
    if closed or not active then return false end
    local ok,value=pcall(fn,...)
    if not ok then A.log('Native.'..op..' ['..pkg..']: '..tostring(value));active=false;return false end
    return op=='tick' and value~=false or op=='intercept' and value==true
   end
   local id=assert(op=='tick' and raw(op,invoke) or raw(op,path,invoke),'subscription failed')
   return own(function()
    if not active then raw('cancel',id);return end
    active=false;raw('cancel',id)
   end)
  end
  function E.on_tick(fn)
   return attempt('on_tick',subscribe,'tick',nil,function(delta,time,controller,world)
    return fn(delta,time and {time=time,controller=controller,world=world} or nil)
   end)
  end
  -- Consume a void Blueprint call BEFORE its body. Ordinary SDK Blueprint hooks
  -- are POST observers even when requested as "pre".
  function E.intercept(path,fn)
   return attempt('intercept '..tostring(path),subscribe,'intercept',path,fn)
  end
  function E.load_class(path,world)
   return attempt('load_class '..tostring(path),function()
    local class,why=raw('load_class',path,world);assert(class,why or 'native class loader returned nil');return valid(class)
   end)
  end
  -- Pending assets are an asynchronous result, not an operational failure.
  function E.asset(path)
   assert(not closed,'scope closed')
   local asset,why=raw('load_asset',path)
   if why then error(why,0) end
   return asset
  end
  function E.release_asset(path)
   local ok,why=raw('release_asset',path);assert(ok,why);return ok
  end
  function E.scene_actor(row,world)
   return attempt('scene_actor '..tostring(row.source),function()
    local t=row.transform or {}
    local actor,why=raw('scene_actor',valid(R.native_call('StaticFindObject','/Script/Engine.Actor')),valid(world),
     t[1],t[2],t[3],t[5],t[4],t[6],t[7],t[8],t[9],row.source,table.concat(row.scene_components or {},'|'))
    assert(actor,why or 'geometry actor unavailable');return actor==true and true or valid(actor)
   end)
  end
  function E.read_bytes(actor,property)
   return attempt('read_bytes '..tostring(property),function()
    local bytes,why=raw('read_bytes',valid(actor),property);assert(type(bytes)=='string',why or 'native byte array unavailable');return bytes
   end)
  end
  function E.teleport_trace()
   return attempt('teleport_trace',function() local ok,why=raw('teleport_trace');assert(ok,why or 'native teleport trace unavailable');return ok end)
  end
  function E.teleporter_exit(zone,exit)
   return attempt('teleporter_exit',function() return raw('teleporter_exit',valid(zone),valid(exit)) end)
  end
  local function spawn(op,class,world,position,initialize)
   return attempt('spawn '..tostring(class),function()
    if type(class)=='string' then local why;class,why=raw('load_class',class,world);assert(class,why or 'native class loader returned nil') end
    local init=initialize and function(actor) initialize(actor);return true end or nil
    local actor,why=raw(op,valid(class),valid(world),position.X,position.Y,position.Z,position.Yaw or 0,position.Pitch or 0,position.Roll or 0,position.ScaleX or 1,position.ScaleY or 1,position.ScaleZ or 1,init)
    assert(actor,why or 'native spawn returned nil');return valid(actor)
   end)
  end
  function E.spawn(...) return spawn('spawn',...) end
  -- The same native construction transaction, without server replication or a server-only GameMode.
  function E.spawn_local(...) return spawn('spawn_local',...) end
  -- Resource cleanup can follow scope cancellation during package teardown.
  -- Destruction still passes through the host's actual Object/world checks.
  function E.destroy(actor)
   local ok,value,why=pcall(function()
    if not actor or not actor:IsValid() then return true end
    return raw('destroy',actor)
   end)
   if ok and value==true then return true end
   local reason=ok and (why or 'native destruction returned '..tostring(value)) or value
   A.log('Native.destroy ['..pkg..']: '..tostring(reason));return nil,reason
  end
  function E.clipboard(operation,bytes)
   return attempt('clipboard '..tostring(operation),function()
    return R.native_call('WitchItNativeClipboard',operation,bytes)
   end)
  end
  function E.update_file(operation,bytes)
   return attempt('update_file '..tostring(operation),function()
    return R.native_call('WitchItNativeUpdateFile',operation,bytes)
   end)
  end
  function E.close()
   if closed then return true end
   closed=true
   for i=#stops,1,-1 do
    local ok,err=pcall(stops[i]); if not ok then A.log('Native.close ['..pkg..']: '..tostring(err)) end
   end
   stops={}; return true
  end
  A.own(E.close,E.close)
  return E
 end
 return Native
end
return M
