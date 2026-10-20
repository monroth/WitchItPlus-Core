-- Native file observations and the current GM file. Subscribers cannot veto IO.
local M={}
function M.new(log)
 local subscribers,last={},nil
 local F={}
 function F.current(owner)return last and last.owner_key==owner and last or nil end
 function F.clear(owner)if not owner or last and last.owner_key==owner then last=nil end end
 function F.observe(fn,existing)
  local row={fn=fn};subscribers[#subscribers+1]=row
  if existing and last then fn(last,true)end
  return function()
   row.fn=nil;for i,v in ipairs(subscribers)do if v==row then table.remove(subscribers,i);break end end
   return true
  end
 end
 local function notify(e,existing)
  for _,row in ipairs({table.unpack(subscribers)})do if row.fn then
   local ok,why=pcall(row.fn,e,existing)
   if not ok then log('LayoutFiles '..e.kind..' '..e.phase..' target='..tostring(e.target_path)..': '..tostring(why))end
  end end
 end
 -- Enabling Core does not replay a disk read. Publish the GM's current file as
 -- an existing snapshot so already-enabled observers also bind it once.
 function F.refresh(e)
  if last and last.owner_key==e.owner_key and last.name==e.name and last.target_path==e.target_path then return end
  last=e;notify(e,true)
 end
 function F.emit(e)
  if e.complete and e.success and not e.autosave and not e.recovery then last=e end
  notify(e)
  return true
 end
 return F
end
return M
