-- The lobby/join inventory uses Core's existing WIR2 RPC transport.
-- Core also shows the actionable mismatch dialog when a peer inventory changes.
local M={}
function M.describe(snapshot)
 if not snapshot or not snapshot.ready then return '' end
 local lines={};local peers=snapshot.peers or {}
 for _,peer in ipairs(peers)do
  for _,problem in ipairs(peer.problems or {})do lines[#lines+1]=problem end
 end
 if #lines>0 then
  return 'Update required:\n'..table.concat(lines,'\n')
 end
 if #peers==0 then return 'No other players connected.' end
 for _,peer in ipairs(peers)do
  if not peer.packages_compatible then return 'Checking other players’ mods…' end
 end
 return 'Mod versions match.'
end
return M
