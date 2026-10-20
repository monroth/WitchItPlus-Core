-- File mechanics live in the runtime so config is available before Core starts.
local M={}
function M.new(api) return {open=api.storage,config=api.config,save_settings=api.save_settings,json=api.json} end
return M
