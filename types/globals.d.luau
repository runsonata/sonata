--[[
	Globals Sonata adds to every script, for the Luau Language Server.
	Written into .sonata/types by `sn init --lsp`; edits are lost when they are regenerated.
]]

--[=[
	The arguments given to the program, as a read-only array.

	Under `sn run`, the arguments after `--`. In a built program, every argument after the
	program's name. The same array is `process.args`.
]=]
declare args: { string }

--[=[
	What the running program knows about itself. For Sonata's build details see `@sonata/sonata`.
]=]
declare sonata: {
	--- Sonata's version, e.g. `"0.1.8"`.
	version: string,
	--- `"<os>-<arch>"`: `"linux-x64"`, `"macos-arm64"`, `"windows-x64"`, ...
	platform: string,
	--- The module path of the entry script.
	entry: string,
	--- `true` inside a program made by `sn build`, `false` under `sn run`.
	bundled: boolean,
	--- The path of the program's executable. Only set when `bundled` is `true`.
	executable: string?,
	project: {
		--- The name from the manifest.
		name: string,
		--- The version from the manifest.
		version: string,
		--- Under `sn run`, the project folder. In a built program, the folder that contains the executable.
		root: string,
	},
}
