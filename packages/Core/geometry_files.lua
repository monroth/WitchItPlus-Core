-- One geometry companion observer. Actor application stays with its editor.
local M={}
local dir=assert(debug.getinfo(1,'S').source:match('^@(.+[\\/])'))
local IO=dofile(dir..'companion_io.lua')
local function copy(t)local r={}for k,v in pairs(t)do r[k]=v end;return r end
local function equal(a,b)for k in pairs(a)do if not b[k]then return false end end;for k in pairs(b)do if not a[k]then return false end end;return true end
function M.new(api,X,codec,Store,state)
 local D,F=api.map_document,api.shared('Core').layout_files
 local saves,pending={}
 local function log(s)X.log('GeometryFiles',s)end
 local function document()local token,v=D.capture();if token and D.current(token)then return v end end
 local function read(e)
  local level=e.world or assert(document(),'current geometry document unavailable').world
  local bytes=IO.read(e.target_path..'.faphidden',1100000)
  if not bytes then return {}end
  if bytes:sub(1,21)=='FAPP_GEOMETRY_MAIN_V2' then return Store.decode(bytes,{world=level,name=e.name,target_path=e.target_path},'manual')end
  return codec.decode(bytes,level)
 end
 local function incoming()
  assert(pending and not pending.error,pending and pending.error)
  if not pending.rows then pending.rows=read(pending.event)end
  return copy(pending.rows)
 end
 local G={}
 function G.read(s)
  -- Reload retries the unread source, even after the native Save As moved on.
  if pending then pending.error=nil;return incoming(),pending.unpublished==true end
  return s.path and read({world=s.level,name=s.documentName,target_path=s.path})or {},false
 end
 local function snapshot()
  local v=document();local s=state()
  if not v or v.role~='host'or v.document_kind=='not_creative'then return end
  if s and s.ownerGeneration==v.owner_generation and s.documentGeneration==v.document_generation then
   assert(not s.busy,'geometry edit running; companion preserved')
   assert(not s.readError,s.readError)
   return {rows=copy(s.desired or {}),revision=s.geometryRevision or 0,document=v,state=s}
  end
  local rows=pending and incoming()or v.target_path and read(v)or {}
  return {rows=rows,revision=0,document=v}
 end
 local function request(e)
  pending={event=e}
  if X.Suppression and X.Suppression.invalidate then X.Suppression.invalidate()end
  if X.wake then X.wake()end
 end
 local stop=F.observe(function(e,existing)
  if existing then request(e);return end
  if e.kind=='load'and e.complete and e.success then request(e)
  elseif e.kind=='save'then
   if e.phase=='entry'then
    local ok,s=pcall(snapshot)
    if ok then saves[e.serial]=s else
     -- The unread original remains the only source; Save As cannot replace it
     -- with the new destination's absent file on the next reload.
     if not pending then local v=document();if v and v.target_path then pending={event=v,error=tostring(s)}end end
     log('snapshot failed; companion preserved: '..tostring(s))
    end
   elseif e.complete then
    local snap=saves[e.serial];saves[e.serial]=nil
    if e.success and not e.autosave and pending then pending.unpublished=true end
    if e.success and snap and snap.document.actor_key==e.owner_key then
     local s=state()
     if not e.autosave then
      if pending then pending.unpublished=true end
      if s then s.dirty=true end
     end
     local ok,why=pcall(function()
      local target={world=snap.document.world,name=e.name,target_path=e.target_path}
      local source={world=snap.document.world,document_kind=snap.document.document_kind,source_name=snap.document.name,revision=snap.revision}
      local done,reason=Store.write(target,snap.rows,function()return api.is_live()end,'manual',source,e.serial)
      assert(done,reason)
     end)
     if not ok then
      if s then s.fileError=e.target_path..'.faphidden: '..tostring(why)..'; save again to retry' end
      log(e.target_path..'.faphidden: '..tostring(why)..'; save again to retry')
      if not s and not pending then pending={event=snap.document,rows=snap.rows,unpublished=true}end
     else
      if not e.autosave then
       if s and not s.readError then pending=nil
       elseif pending then pending.event={world=snap.document.world,name=e.name,target_path=e.target_path};pending.rows=copy(snap.rows);pending.unpublished=false end
       if s and s==snap.state then s.committed=copy(snap.rows);s.dirty=not equal(s.desired,s.committed);s.fileError=nil end
      end
      log('saved '..e.target_path..'.faphidden')
     end
    end
   end
  end
 end,true)
 function G.failed(s,why)s.readError=tostring(why);if pending then pending.error=s.readError end end
 function G.loaded(s)s.readError=nil end
 function G.clear()saves={};pending=nil end
 local cancel=D.on_change(function(_,reason)if reason=='new'then G.clear()end end)
 function G.close()G.clear();stop();if cancel then cancel()end;return true end
 api.own(G.close,G.close)
 return G
end
return M
