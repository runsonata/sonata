--[[
	Type definitions for @sonata/luau, used by the Luau Language Server.
	Written into .sonata/types by `sn init --lsp`; edits are lost when they are regenerated.
]]

--[=[
	Compile, load and run Luau code.

	Two levels of isolation:

	- **In this VM** (`compile`, `check`, `load`, `eval`, `env`): the code shares this VM, its
	  values and its memory. Its globals can be any table you choose. There are no time or memory
	  limits, so only use it for code you trust.
	- **Isolated VMs** (`luau.vm`): a separate VM with its own globals and heap, an optional memory
	  limit and an optional time limit per call. Values crossing over are copied; functions become
	  proxies that call back into their own VM. Use this for untrusted code.

	A chunk is source code, or a buffer of bytecode made by `luau.compile`. Bytecode isn't
	verified: only load bytecode you compiled yourself.
]=]
local luau = {}

export type CompileOptions = {
	--- The chunk name used in error messages. Default `"chunk"`.
	name: string?,
	--- 0, 1 (default) or 2. 2 inlines functions and unrolls loops.
	optimizationLevel: number?,
	--- 0, 1 (default: line numbers) or 2 (also local names).
	debugLevel: number?,
}

export type LoadOptions = {
	--- The chunk name used in error messages and stack traces. Default `"chunk"`.
	name: string?,
	--[=[
		The chunk's global table. Without it, the chunk gets a fresh table that reads through to
		the caller's globals, so it sees everything but its own globals stay in that table.
	]=]
	env: { [any]: any }?,
	optimizationLevel: number?,
	debugLevel: number?,
}

export type EnvOptions = {
	--- Standard libraries to include. Default: all of `luau.libraries` except `debug`.
	libraries: { string }?,
	--- Extra globals, shared by reference.
	globals: { [string]: any }?,
}

export type VMOptions = {
	--- Used as the chunk name for `vm:run` and `vm:try`, and by `tostring(vm)`. Default `"vm"`.
	name: string?,
	--- Standard libraries the VM gets. Default: all of `luau.libraries`.
	libraries: { string }?,
	--[=[
		Default `true`: the libraries and base globals are read-only, and chunks write their
		globals to a separate table (which `_G` also refers to). `false` leaves everything writable.
	]=]
	sandbox: boolean?,
	--- Globals to copy into the VM. Functions become proxies.
	globals: { [string]: any }?,
	--- Bytes the VM may use in total, the standard library included. `0` or `nil`: no limit.
	memoryLimit: number?,
	--- Seconds each call into the VM may run. `0` or `nil`: no limit.
	timeout: number?,
	--- Where the VM's `print` goes: a function receiving each line, or `false` to discard it. Default: stdout.
	print: ((line: string) -> ()) | false | nil,
	--- Modules the VM can `require` by name, as source code.
	modules: { [string]: string }?,
	optimizationLevel: number?,
	debugLevel: number?,
}

export type VMError = {
	--[=[
		- `"syntax"`: the chunk doesn't compile
		- `"runtime"`: it raised an error
		- `"timeout"`: it ran past the time limit
		- `"memory"`: it ran out of memory
		- `"yield"`: it yielded outside of a coroutine
	]=]
	kind: "syntax" | "runtime" | "timeout" | "memory" | "yield",
	--- The error message; for a non-string error, a description of it.
	message: string,
	--- The error value itself, copied out of the VM.
	value: any,
	--- The VM's stack trace where the error happened (`""` for syntax errors).
	traceback: string,
}

export type VMStats = {
	--- Bytes in use.
	memory: number,
	--- The most bytes in use at once.
	peakMemory: number,
	--- Present when there is a memory limit.
	memoryLimit: number?,
	--- Present when there is a time limit.
	timeout: number?,
	--- Calls into the VM so far.
	calls: number,
	--- Calls that failed.
	errors: number,
	--- Calls that ran past the time limit.
	timeouts: number,
}

--[=[
	An isolated Luau VM. Values passed in or out are copied (tables deeply, keeping cycles; not
	their metatables). Functions cross as proxies, and turn back into themselves when they return
	to their own VM. Userdata and threads can't cross.

	A path is a global name, or names separated by dots (`"config.window.width"`).
]=]
export type VM = {
	--- Compiles and runs a chunk, with `...` as its arguments. Returns its results; raises on any failure.
	run: (self: VM, chunk: string | buffer, ...any) -> ...any,
	--- Like `run`, but returns `true` and the results, or `false` and a `VMError`.
	try: (self: VM, chunk: string | buffer, ...any) -> (boolean, ...any),
	--- Compiles a chunk into a function that runs it in the VM each time it is called.
	load: (self: VM, chunk: string | buffer, name: string?) -> (...any) -> ...any,
	--- Calls the function at `path` with the arguments and returns its results.
	call: (self: VM, path: string, ...any) -> ...any,
	--- The value at `path`, or `nil` if it or a table on the way is missing.
	get: (self: VM, path: string) -> any,
	--- Sets the value at `path`. Every table on the way must exist.
	set: (self: VM, path: string, value: any) -> (),
	--- The names of the VM's globals, sorted.
	globals: (self: VM) -> { string },
	--- Adds (or replaces) a module the VM can `require`. Raises if it doesn't compile.
	defineModule: (self: VM, name: string, source: string | buffer) -> (),
	--- Changes the time limit for later calls. `nil` or `0` removes it.
	setTimeout: (self: VM, seconds: number?) -> (),
	--- Changes the memory limit. `nil` or `0` removes it.
	setMemoryLimit: (self: VM, bytes: number?) -> (),
	stats: (self: VM) -> VMStats,
	--- Runs a full garbage collection in the VM and returns the bytes still in use.
	collect: (self: VM) -> number,
	--- Replaces the VM with a fresh one made from the same options. Proxies of its old functions stop working.
	reset: (self: VM) -> (),
	--- Frees the VM. Calling into it afterwards raises. Closing twice is fine.
	close: (self: VM) -> (),
	isClosed: (self: VM) -> boolean,
}

--- The standard libraries a VM or environment can be given, e.g. `{ "bit32", "buffer", ... }`.
luau.libraries = (nil :: any) :: { string }

--- Luau's version, e.g. `"0.739"`. `""` if unknown.
luau.version = (nil :: any) :: string

--- Compiles source code into bytecode. Raises on a syntax error.
function luau.compile(source: string, options: CompileOptions?): buffer
	return nil :: any
end

--- Whether the source compiles; if not, also the error message (`"<name>:<line>: <message>"`).
function luau.check(source: string, name: string?): (boolean, string?)
	return nil :: any
end

--- Compiles a chunk into a function running in this VM. On a syntax error, returns `nil` and the message.
function luau.load(chunk: string | buffer, options: LoadOptions?): (((...any) -> ...any)?, string?)
	return nil :: any
end

--[=[
	Compiles and runs source code in this VM, with `...` as its arguments, and returns its
	results. `env` works as in `luau.load`. Raises on any error. The code may yield.
]=]
function luau.eval(source: string, env: { [any]: any }?, ...: any): ...any
	return nil :: any
end

--[=[
	A fresh global table holding only safe builtins: the base functions (without `getfenv`,
	`setfenv` and `require`), the standard libraries and `_G`. Give it to `luau.load` or
	`luau.eval` as `env`. This limits what code can reach, not how long it runs or how much memory
	it uses: for that, use `luau.vm`.
]=]
function luau.env(options: EnvOptions?): { [any]: any }
	return nil :: any
end

--- Creates an isolated VM.
function luau.vm(options: VMOptions?): VM
	return nil :: any
end

return luau
