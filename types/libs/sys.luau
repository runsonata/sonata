--[[
	Type definitions for @sonata/sys, used by the Luau Language Server.
	Written into .sonata/types by `sn init --lsp`; edits are lost when they are regenerated.
]]

--[=[
	Facts about the machine the program runs on, plus clocks and sleeping. Facts about the running
	program itself are in `@sonata/process`.

	Queries that can fail return `nil` instead of raising.
]=]
local sys = {}

export type OS = "windows" | "macos" | "linux" | "freebsd" | "android" | "unknown"

--- `"windows"`, `"macos"`, `"linux"`, `"freebsd"`, `"android"` or `"unknown"`.
sys.os = (nil :: any) :: OS

--- The machine's architecture: `"x86_64"`, `"aarch64"`, `"x86"`, `"arm"`, `"riscv64"`, ...
sys.arch = (nil :: any) :: string

--- `"little"` or `"big"`.
sys.endianness = (nil :: any) :: "little" | "big"

--- `"\n"`, or `"\r\n"` on Windows.
sys.eol = (nil :: any) :: string

--- `"/"`, or `"\\"` on Windows.
sys.pathSeparator = (nil :: any) :: string

--- `":"`, or `";"` on Windows, as used in PATH.
sys.pathDelimiter = (nil :: any) :: string

--- `"/dev/null"`, or `"NUL"` on Windows.
sys.devNull = (nil :: any) :: string

--- The machine's host name.
function sys.hostname(): string?
	return nil :: any
end

--- The user the program runs as.
function sys.username(): string?
	return nil :: any
end

--- That user's home directory.
function sys.homeDir(): string?
	return nil :: any
end

--- The temporary directory.
function sys.tempDir(): string?
	return nil :: any
end

--- The OS or kernel release, e.g. `"6.8.0-45-generic"`.
function sys.osVersion(): string?
	return nil :: any
end

--- The number of logical processors, at least 1.
function sys.cpuCount(): number
	return nil :: any
end

--- Bytes of physical memory.
function sys.totalMemory(): number?
	return nil :: any
end

--- Bytes of memory available to programs.
function sys.freeMemory(): number?
	return nil :: any
end

--- Bytes per memory page.
function sys.pageSize(): number?
	return nil :: any
end

--- Seconds since the machine booted.
function sys.uptime(): number?
	return nil :: any
end

--- The 1, 5 and 15 minute load averages. `nil` on Windows.
function sys.loadAverage(): { number }?
	return nil :: any
end

--- Every constant and every query of this library, collected into one table (with the same names).
function sys.info(): { [string]: any }
	return nil :: any
end

--- Wall-clock time: seconds since the Unix epoch, with a fractional part.
function sys.time(): number
	return nil :: any
end

--- Monotonic seconds from an arbitrary starting point. Use it to measure durations.
function sys.clock(): number
	return nil :: any
end

--- Pauses the whole program, blocking every task. To pause only the current task, use `task.wait`.
function sys.sleep(seconds: number): () end

return sys
