-- The only on-wire package allowlist. Private/local packages never leave this PC.
local M={ids={Core='core',BaseWorldPlus='baseworldplus',FillAPotPlus='fillapotplus',MatchRulesPlusPlus='matchrulesplusplus'},
 names={core='Core',baseworldplus='Editor tweaks',fillapotplus='Fill a Pot+',matchrulesplusplus='Match Rules +'}}
function M.version(s)
 if type(s)~='string' or #s>32 then return end
 local a,b,c=s:match('^(%d+)%.(%d+)%.(%d+)$')
 if not a then return end
 for _,v in ipairs({a,b,c})do if #v>1 and v:sub(1,1)=='0' or tonumber(v)>999999 then return end end
 return {tonumber(a),tonumber(b),tonumber(c)}
end
function M.compare(a,b)
 a=assert(M.version(a),'invalid version');b=assert(M.version(b),'invalid version')
 for i=1,3 do if a[i]~=b[i]then return a[i]>b[i] and 1 or -1 end end
 return 0
end
function M.validate(packages,empty)
 assert(type(packages)=='table' and getmetatable(packages)==nil,'packages must be an object')
 local n=0
 for id,v in pairs(packages)do assert(M.names[id] and M.version(v),'invalid public package/version');n=n+1 end
 assert(empty and n==0 or not empty and packages.core and n<=4,'complete public set requires core')
 return packages
end
-- Network IDs stay stable; the updater addresses canonical repository packages.
function M.canonical(packages,empty)
 M.validate(packages,empty)
 local out={};for id,wire in pairs(M.ids)do if packages[wire]then out[id]=packages[wire]end end
 return out
end
function M.rows(rows)
 local result={}
 for _,r in ipairs(rows or {})do if M.ids[r.id]then
  result[#result+1]={id=r.id,version=r.version,protocol_version=r.protocol_version,required_on_all_players=r.required_on_all_players}
 end end
 return result
end
function M.wire(rows,json)return json.array(M.rows(rows))end
function M.map(rows)
 local result={};for _,r in ipairs(M.rows(rows))do result[M.ids[r.id]]=r.version end;return result
end
function M.details(host,local_)
 M.validate(host);local rows,compatible={},true
 for _,id in ipairs({'core','baseworldplus','fillapotplus','matchrulesplusplus'})do
  if host[id] or local_[id]then
   local v=local_[id];local mark=not v and '✗' or not host[id] and '↑' or M.compare(v,host[id])==0 and '✓' or M.compare(v,host[id])>0 and '↑' or '↓'
   if mark~='✓'then compatible=false end
   rows[#rows+1]={id=id,name=M.names[id],host=host[id],local_version=v,mark=mark}
  end
 end
 return rows,compatible
end
function M.refusal(host,local_)
 local rows=M.details(host,local_);local names={}
 for _,r in ipairs(rows)do if r.mark~='✓'then names[#names+1]=r.name end end
 if #names>0 then return 'Mods differ from host: '..table.concat(names,', ')..'. Use Sync mods with host.' end
end
function M.mismatch(localRows,remoteRows,localHost)
 local remote,seen={},{}
 for _,r in ipairs(remoteRows)do
  assert(type(r)=='table'and type(r.id)=='string','invalid package row')
  -- Older WIR2 clients included local/private rows. Discard them on ingress;
  -- they never reach comparison text, snapshots or our outbound inventory.
  if M.ids[r.id]then
   assert(not seen[r.id] and M.version(r.version) and r.protocol_version==2
    and type(r.required_on_all_players)=='boolean','invalid public inventory')
   seen[r.id]=true;remote[M.ids[r.id]]=r.version
  end
 end
 M.validate(remote)
 local rows=M.details(remote,M.map(localRows));local errors={}
 for _,r in ipairs(rows)do if r.mark~='✓'then
  local localSide=localHost and 'Host' or 'Client';local remoteSide=localHost and 'Client' or 'Host'
  if not r.host or not r.local_version then
   errors[#errors+1]=(not r.local_version and localSide or remoteSide)..' must enable '..r.name..' '..(r.host or r.local_version)
  else
   local older=M.compare(r.local_version,r.host)<0 and localSide or remoteSide
   errors[#errors+1]=older..' must update '..r.name..' to '..(M.compare(r.local_version,r.host)>0 and r.local_version or r.host)..' (host '..(localHost and r.local_version or r.host)..', client '..(localHost and r.host or r.local_version)..')'
  end
 end end
 return errors,remote
end
function M.summary(packages)
 local out={};for _,id in ipairs({'core','baseworldplus','fillapotplus','matchrulesplusplus'})do
  if packages[id]then out[#out+1]=M.names[id]..' '..packages[id]end
 end;return table.concat(out,', ')
end
return M
