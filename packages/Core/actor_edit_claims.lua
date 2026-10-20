-- Pure shared actor reservations. Scope comes from the common native document;
-- claims survive same-world document replacement until verified rollback/release.
local M={}
local function literal(v)return type(v)=='string' and #v>0 and #v<=4096 and not v:find('[%c]')end
local fields={'world_key','world_generation','core_epoch','owner_generation','document_generation'}
local function copy(v)local r={}for _,k in ipairs(fields)do r[k]=v[k]end;return r end
local function scope(v)
 assert(type(v)=='table' and literal(v.world_key),'actor claim world required')
 for _,k in ipairs({'world_generation','core_epoch','owner_generation','document_generation'})do assert(math.type(v[k])=='integer' and v[k]>=0,'actor claim generation required')end
 return copy(v)
end
local function same(a,b)for _,k in ipairs(fields)do if a[k]~=b[k]then return false end end;return true end
local function resource(v,path)return v.world_key..'\0'..v.world_generation..'\0'..v.core_epoch..'\0'..path end
function M.new(legacy)
 local reserved,records={},{};local service={}
 function service.blocked(v,path)
  v=scope(v);assert(literal(path),'literal actor path required');return reserved[resource(v,path)]~=nil
 end
 function service.for_owner(api,D,L,lane)
  assert(type(lane)=='string' and lane:match('^[%w_]+$') and type(api.is_live)=='function','owned actor claim facade required')
  local facade={};local leases={};local closed=false
  local function live()return not closed and api.is_live()==true and not L.transition end
  local function capture()
   if not live()then return end;local token,v=D.capture()
   if not token or not v or not D.current(token)or not live()or v.role~='host'then return end
   return token,scope(v)
  end
  local function canonical(h)
   local r=records[h];return r and r.facade==facade and reserved[r.key]==r and r or nil
  end
  function facade.current(h)
   local r=canonical(h);if not r or r.pending or not live()then return false end
   local token,v=capture();if not v or not same(v,r.scope)then return false end
   local ok,yes=pcall(r.current)
   return ok and yes==true and canonical(h)==r and live()and D.current(token)==true
  end
  function facade.acquire(path,owner,current)
   assert(owner==api.id and literal(path)and type(current)=='function','actor claim owner/path required')
   local token,v=capture();if not v then return end
   local key=resource(v,path);if reserved[key]then return end
   local h={};local r={facade=facade,key=key,path=path,scope=v,current=current,pending=true};records[h]=r;reserved[key]=r;leases[h]=true
   local ok,yes=pcall(function()return current()==true and (not legacy or legacy(v,path)~=true)end)
   if not ok or yes~=true or not live()or not D.current(token)or canonical(h)~=r then
    if canonical(h)==r then reserved[key]=nil;records[h]=nil;leases[h]=nil end;return
   end
   r.pending=false;return h
  end
  function facade.release(h)
   local r=canonical(h);if not r then return false end
   records[h]=nil;reserved[r.key]=nil;leases[h]=nil;return true
  end
  function facade.retire()
   local paths={};for h in pairs(leases)do local r=canonical(h);if r then paths[#paths+1]=r.path end end
   if #paths>0 then table.sort(paths);return false,'actor claims '..lane..': unreleased '..table.concat(paths,', ') end;closed=true;return true
  end
  function facade.abandon()
   closed=true;local old=leases;leases={}
   for h in pairs(old)do local r=canonical(h);if r then records[h]=nil;reserved[r.key]=nil end end;return true
  end
  api.own(facade.retire,facade.abandon)
  return facade
 end
 return service
end
return M
