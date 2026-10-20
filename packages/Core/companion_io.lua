-- Independent bounded companion IO; one file is published at a time.
local M={}
local function literal(v)return type(v)=='string' and #v>0 and #v<=4096 and not v:find('[%c]')end
local function number(v)return type(v)=='number' and v%1==0 and v>=0 and v<=2147483647 end
function M.read(path,limit)
 local f,why,code=io.open(path,'rb')
 if not f then assert(code==2,why or 'companion unavailable');return nil end
 local ok,bytes=pcall(f.read,f,limit+1);local closed,value=pcall(f.close,f)
 assert(ok,bytes);assert(closed and value==true,'companion close failed')
 assert(type(bytes)=='string' and #bytes<=limit,'companion read failed or exceeds limit: '..path)
 return bytes
end
function M.writeBytes(file,bytes,current,operation,preserveExisting)
 assert(literal(file) and type(bytes)=='string' and #bytes<=1100000,'invalid companion bytes')
 assert(type(current)=='function','owned IO guard required')
 local suffix
 if type(operation)=='number' then
  assert(number(operation),'file serial required');suffix='.file-'..operation
 else
  assert(number(operation.owner_generation) and number(operation.document_generation) and number(operation.operation_serial),'operation identity required')
  suffix='.op-'..operation.owner_generation..'-'..operation.document_generation..'-'..operation.operation_serial
 end
 local tmp,backup=file..suffix..'.tmp',file..suffix..'.bak'
 local function check() assert(current(),'geometry IO owner retired') end
 local function read(path)
  check();local f,err,code=io.open(path,'rb')
  if not current() then if f then pcall(f.close,f) end;error('geometry IO owner retired',0) end
  if not f then if code==2 then return nil end;error(err or 'geometry file availability unknown',0) end
  local ok,data=pcall(f.read,f,1100001)
  local closed,cerr=pcall(f.close,f)
  if not ok then error(data,0) end
  if not closed or cerr~=true then error(cerr or 'geometry file close failed',0) end
  check();assert(type(data)=='string' and #data<=1100000,'geometry file too large');return data
 end
 local old=read(file)
 if preserveExisting and old~=nil then
  if old==bytes then return true end
  return false,'existing catalog metadata differs; preserved'
 end
 if read(tmp)~=nil then return false,'foreign geometry temporary exists' end
 if read(backup)~=nil then return false,'foreign geometry backup exists' end
 check()
 local f,err=io.open(tmp,'wb');if not f then return false,err end
 -- Opening may reenter the document producer. Only this returned descriptor
 -- may be closed after revocation; no old write is authorized by open alone.
 if not current() then pcall(f.close,f);error('geometry IO owner retired',0) end
 local ok,written,why=pcall(f.write,f,bytes)
 local closeOK,closed,cerr=pcall(f.close,f)
 if not ok then return false,written end
 if not closeOK then return false,closed end
 if not written or not closed then return false,why or cerr end
 check()
 if read(file)~=old then return false,'geometry destination changed' end
 if read(tmp)~=bytes then return false,'geometry temporary changed' end
 check()
 if old~=nil then
  if read(backup)~=nil then return false,'foreign geometry backup exists' end
  check();local moved,merr=os.rename(file,backup);check()
  if not moved then return false,merr end
 end
 -- Never restore a destination from a stale operation. A retained owned
 -- backup is a partial-save recovery fact, not authority over a new document.
 if read(file)~=nil then return false,'geometry destination appeared' end
 check();local installed,ierr=os.rename(tmp,file);check()
 if not installed then
  if old~=nil and read(file)==nil and read(backup)==old then
   local restored,rerr=os.rename(backup,file)
   if not restored then return false,tostring(ierr)..'; original retained at '..backup..': '..tostring(rerr)end
  end
  return false,ierr
 end
 if read(file)~=bytes then return false,'geometry publication changed' end
 if old~=nil then
  if read(backup)~=old then return false,'geometry backup changed' end
  check();local removed,rerr=os.remove(backup);check()
  if not removed then return false,rerr end
 end
 return true
end
return M
