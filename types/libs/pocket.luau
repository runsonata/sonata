--[[
	Type definitions for @sonata/pocket, used by the Luau Language Server.
	Written into .sonata/types by `sn init --lsp`; edits are lost when they are regenerated.
]]

--[=[
	Read-only files that travel with the program: whatever the package lists in `build.assets`.

	Paths are relative to the root of the package the calling script belongs to, use `/`, and may
	not leave the package.
]=]
local pocket = {}

--- `true` inside a program made by `sn build`, `false` under `sn run`.
pocket.bundled = (nil :: any) :: boolean

--- The contents of an asset. Raises an error if there is no such asset.
function pocket.read(path: string): string
	return nil :: any
end

--- The contents of an asset, or `nil` if there is no such asset.
function pocket.tryRead(path: string): string?
	return nil :: any
end

--- Whether an asset exists.
function pocket.exists(path: string): boolean
	return nil :: any
end

--- Every asset of the package, sorted, or only those under `folder`.
function pocket.list(folder: string?): { string }
	return nil :: any
end

return pocket
