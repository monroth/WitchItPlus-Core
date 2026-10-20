-- Observe actual native files. Each companion owns its own IO and errors.
local M={}
function M.install(api,Core,Owner)
 local alive=true;local lastOutcome
 local bridge=api.native_boundaries(function(phase,id,owner,requested,compressed,autosave,target,success,admitted,name,backup)
  if not alive then return true end
  if phase=='complete' and success and not autosave then
   local ok,why=pcall(Owner.file_saved,{owner_key=owner,name=name,target_path=target})
   if not ok or why~=true then api.log('LayoutFiles: native saved identity '..tostring(target)..': '..tostring(ok and 'current GM not yet observed' or why))end
  end
  if phase=='disk_outcome' then lastOutcome={state=name,target_path=target,backup_path=backup,serial=id}end
  local _,view=api.map_document.capture()
  Core.layout_files.emit({world=view and view.world,kind='save',phase=phase,serial=id,owner_key=owner,requested_name=requested,
   compressed=compressed,autosave=autosave,target_path=target,success=success,name=name,complete=phase=='complete'})
  return true -- observers never admit or veto vanilla IO
 end,function(phase,id,owner,target,compressed,recovery,intent,readerDone,readerSuccess,applicationDone,name,world,path)
  if alive then Core.layout_files.emit({kind='load',phase=phase,serial=id,owner_key=owner,target_path=path,
   compressed=compressed,recovery=recovery,name=name,world=world,
   complete=phase=='return',success=readerDone and readerSuccess and applicationDone})end
  return true
 end)
 local function retire()alive=false;return true end
 api.own(retire,retire)
 return {retire=retire,outcome=function()return lastOutcome end}
end
return M
