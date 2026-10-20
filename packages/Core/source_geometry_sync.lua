-- Finite geometry publications on the existing acknowledged WIR2 RPC routes.
-- Only the host publishes. File names select a cache, never the authoritative rows.
local M={PREFIX='FAPSG2:',CHUNK=320,LIMIT=1100000}
local PC='/Game/Blueprints/Player/B_PlayerController.B_PlayerController_C:'
local function hex(s)return(s:gsub('.',function(c)return string.format('%02x',c:byte())end))end
local function unhex(s)return(s:gsub('%x%x',function(c)return string.char(tonumber(c,16))end))end
local function scope(v)return table.concat({v.pc_key,v.local_session,v.remote_session,v.local_revision,v.remote_revision},'|')end
function M.compact(J,d)
 local rows=J.array()
 for _,row in ipairs(d.actors)do rows[#rows+1]={source=row.source,asset=row.asset,transform=row.transform,scene_components=row.scene_components}end
 return J.encode({format=d.format,version=d.version,binding=d.binding,actors=rows},M.LIMIT)
end
function M.new(api,Core,A)
 local Sync=Core.package_sync;local R=Core.runtime;local J=api.json
 local hash=api.module('content_hash');local owner={id=api.id,is_live=api.is_live,own=api.own}
 local publication,receiver;local serial=0;local sent,queue,asked,requests,served={},{},{},{},{}
 local function log(s)api.log('SourceGeometry: '..s)end
 local function pieces(body)
  local parts={};for i=1,#body,M.CHUNK do parts[#parts+1]=hex(body:sub(i,i+M.CHUNK-1))end;return parts
 end
 local function fanout(snapshot)
  if not snapshot then return end
  if not snapshot.host then
   for _,row in ipairs(snapshot.peers or {})do
    local view=Sync.peer(row.pc_key,'core');local id=view and scope(view)
    if view and not asked[id]then asked[id]=true;requests[#requests+1]=view end
   end
   return
  end
  if not publication then return end
  for _,row in ipairs(snapshot.peers or {})do
   local view=Sync.peer(row.pc_key,'core');local id=view and scope(view)
   if view and not sent[id]then
    sent[id]=true;queue[#queue+1]={view=view,publication=publication,kind=publication.clear and 'CLEAR'or 'HEAD',at=1}
    log('host transfer queued peer='..view.pc_key..' hash='..publication.hash..' actors='..publication.count)
   end
  end
 end
 local S={}
 function S.publish(data)
  local body=data and M.compact(J,data)or '{}';local digest=hash(body)
  if publication and publication.hash==digest then return end
  serial=serial+1;sent={};queue={};served={}
  publication={hash=digest,serial=serial,count=data and #data.actors or 0,clear=data==nil,
   HEAD=data and pieces(J.encode(data.binding))or nil,DATA=data and pieces(body)or nil,CLEAR={'7b7d'}}
  fanout(Sync.snapshot())
 end
 function S.reset()publication=nil;receiver=nil;sent={};queue={};asked={};requests={};served={}end
 local function receive(self,value)
  -- Live FString/parameter wrappers are not Lua strings. Match Core/MRPP's
  -- callback conversion instead of discarding every live geometry packet.
  value=R.text(R.unwrap(value))
  if type(value)~='string'or #value>900 or value:sub(1,#M.PREFIX)~=M.PREFIX then return end
  local verb,ss,ts,sr,tr,rev,digest,i,n,body=value:match('^FAPSG2:(%u+):([%x%-]+):([%x%-]+):(%d+):(%d+):(%d+):(%x+):(%d+):(%d+):(%x+)$')
  sr,tr,rev,i,n=tonumber(sr),tonumber(tr),tonumber(rev),tonumber(i),tonumber(n)
  if not rev or rev<1 or rev>2147483647 or #digest~=64 or not n or n<1 or n>math.ceil(M.LIMIT/M.CHUNK)
   or not i or i<1 or i>n or #body>M.CHUNK*2 or #body%2~=0 then return end
  local pc=R.unwrap(self);local view=Sync.accept(pc,ss,ts,sr,tr,'core')
  if not view then return end
  local id=scope(view)
  if verb=='WANT'then
   if not view.host or n~=1 or i~=1 or body~='7b7d'or hash('{}')~=digest or served[id]then return end
   served[id]=true
   -- Host and guest ACKs can commit on different ticks. A guest requests once
   -- after its own route commits, recovering a push sent before it could accept.
   for _,job in ipairs(queue)do if scope(job.view)==id then
    job.kind=job.publication.clear and 'CLEAR'or 'HEAD';job.at=1;return
   end end
   sent[id]=nil;fanout(Sync.snapshot());log('host recipe requested peer='..view.pc_key);return
  end
  if view.host then return end

  if not receiver or receiver.id~=id then receiver={id=id,view=view,serial=0}end
  local holder=receiver
  if rev<holder.serial or rev==(holder.received or 0)then return end
  if rev>holder.serial then holder.serial=rev;holder.hash=digest;holder.HEAD=nil;holder.DATA=nil;holder.elapsed=0 end
  if digest~=holder.hash then return end
  if verb=='CLEAR'then
   if n~=1 or i~=1 or unhex(body)~='{}'or hash('{}')~=digest then return end
   holder.received=rev;holder.HEAD=nil;holder.DATA=nil;A.clear();log('cleared (from host)');return
  end
  if verb~='HEAD'and verb~='DATA'then return end
  local p=holder[verb]
  if not p then p={parts={},count=0,n=n};holder[verb]=p end
  if n~=p.n then return end
  if p.parts[i]then return end
  p.parts[i]=body;p.count=p.count+1
  if p.count~=n then return end
  holder[verb]=nil
  local raw=unhex(table.concat(p.parts));if #raw>M.LIMIT then return end
  if verb=='HEAD'then
   local binding=J.decode(raw)
   local ok,data=pcall(A.cached,binding,digest)
   if not ok then log('bundled recipe unavailable; receiving host rows: '..tostring(data))end
   if ok and data then
    A.apply(pc,data,digest);holder.received=rev;holder.DATA=nil
    log('host recipe matched bundled hash='..digest)
   end
  else
   assert(hash(raw)==digest,'host geometry content hash differs')
   local data=A.decode(raw);A.apply(pc,data,digest);holder.received=rev;holder.HEAD=nil
   log('host recipe received hash='..digest..' actors='..#data.actors)
  end
 end
 function S.receive(self,value)
  local ok,why=pcall(receive,self,value);if not ok then log('host transfer refused: '..tostring(why))end
 end
 function S.advance(delta)
  if receiver and (not Sync.current(receiver.view)or (receiver.elapsed or 0)>120)then receiver=nil end
  if receiver and receiver.serial~=(receiver.received or 0)then receiver.elapsed=(receiver.elapsed or 0)+delta end
  local want=table.remove(requests,1)
  if want and Sync.current(want)then
   local packet=M.PREFIX..'WANT:'..table.concat({want.local_session,want.remote_session,want.local_revision,want.remote_revision,1,hash('{}'),1,1,'7b7d'},':')
   local called,ok,why=pcall(Sync.send,owner,want.pc_key,packet,want)
   if not called or not ok then log('host recipe request canceled: '..tostring(called and why or ok))end
  end
  local job=queue[1];if not job then return end
  if not Sync.current(job.view)then table.remove(queue,1);return end
  -- Bound RPC work per tick; one immutable body shared by all matching peers.
  for _=1,4 do
   local p=job.publication;local part=p[job.kind][job.at]
   if not part then
    if job.kind=='HEAD'then job.kind='DATA';job.at=1;part=p.DATA[1]
    else table.remove(queue,1);return end
   end
   local v=job.view
   local packet=M.PREFIX..job.kind..':'..table.concat({v.local_session,v.remote_session,v.local_revision,v.remote_revision,p.serial,p.hash,job.at,#p[job.kind],part},':')
   assert(#packet<=900,'geometry RPC exceeds channel limit')
   local called,ok,why=pcall(Sync.send,owner,v.pc_key,packet,v)
   if not called or not ok then log('host transfer canceled peer='..v.pc_key..': '..tostring(called and why or ok));table.remove(queue,1);return end
   job.at=job.at+1
  end
 end
 local cancel=Sync.observe(owner,function(snapshot)
  local ok,why=pcall(fanout,snapshot);if not ok then log('host publication failed: '..tostring(why))end
 end)
 api.hook(PC..'ClientSetRevisionNumber',S.receive,{ungated=true})
 api.hook(PC..'ExecuteConsoleCommandFromClient',S.receive,{ungated=true})
 api.own(function()S.reset();if cancel then cancel()end;return true end)
 return S
end
return M
