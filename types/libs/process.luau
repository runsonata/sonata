--[[
	Type definitions for @sonata/process, used by the Luau Language Server.
	Written into .sonata/types by `sn init --lsp`; edits are lost when they are regenerated.
]]

--[=[
	The running program, the child processes it starts, and signals.

	A program that can't be started raises an error; one that exits with a non-zero code does not
	(`ok` is false). Waiting (exec, wait, pipe reads and writes) lets other tasks run.
]=]
local process = {}

--[=[
	A signal name: `"interrupt"` (Ctrl+C), `"terminate"`, `"hangup"`, `"quit"`, `"user1"`, `"user2"`,
	`"resize"`, `"alarm"`, `"continue"`, `"kill"` and `"stop"` (sending only), `"break"` (Windows).
	`"SIGINT"`, `"INT"` and plain numbers work too.
]=]
export type Signal = string | number

export type StdioMode = "pipe" | "inherit" | "null"

export type ExecOptions = {
	--- Working directory of the child.
	cwd: string?,
	--- Added to / overriding the inherited environment.
	env: { [string]: string }?,
	--- Data fed to the child's stdin.
	stdin: string?,
	--- Run through /bin/sh (cmd.exe on Windows); default `false`.
	shell: boolean?,
	--- `"pipe"` (default) captures stdout/stderr, `"inherit"` shares ours.
	stdio: ("pipe" | "inherit")?,
}

export type ExecResult = {
	--- Exited with code 0.
	ok: boolean,
	--- The exit code.
	code: number,
	stdout: string,
	stderr: string,
}

export type SpawnOptions = {
	cwd: string?,
	env: { [string]: string }?,
	shell: boolean?,
	--- Default `"pipe"`.
	stdin: StdioMode?,
	--- Default `"pipe"`.
	stdout: StdioMode?,
	--- Default `"pipe"`.
	stderr: StdioMode?,
	--- Sets all three; the individual options win.
	stdio: StdioMode?,
	--- Own session / process group: survives our Ctrl+C (default `false`).
	detached: boolean?,
}

export type ExitStatus = {
	ok: boolean,
	--- The exit code; 128 + signal for a child killed by a signal.
	code: number,
	--- The signal that killed the child, if any.
	signal: string?,
}

export type Pipe = {
	--- Writes to the child's stdin. Returns `true`, or `nil, err`.
	write: (self: Pipe, data: string) -> (boolean?, string?),
	--- Closes the pipe; on stdin this sends end-of-file.
	close: (self: Pipe) -> (),
	--- Whatever is available, up to `max` bytes (default 64 KiB). `nil` at end of file.
	read: (self: Pipe, max: number?) -> string?,
	--- The next line. `nil` at end of file.
	readLine: (self: Pipe, keepEnding: boolean?) -> string?,
	--- Everything until end of file.
	readAll: (self: Pipe) -> string,
}

export type Child = {
	--- The child's process ID.
	pid: number,
	--- Set when stdin is `"pipe"`.
	stdin: Pipe?,
	--- Set when stdout is `"pipe"`.
	stdout: Pipe?,
	--- Set when stderr is `"pipe"`.
	stderr: Pipe?,
	--- Waits for the child to exit. Returns its status, or `nil, "timed out"`.
	wait: (self: Child, timeout: number?) -> (ExitStatus?, string?),
	--- `nil` while it runs; the same table as `wait()` once it has exited.
	status: (self: Child) -> ExitStatus?,
	--- Sends a signal (default `"terminate"`).
	kill: (self: Child, signal: Signal?) -> boolean,
}

--- This process's ID.
process.pid = (nil :: any) :: number

--- The parent's process ID (`nil` if it can't be determined).
process.ppid = (nil :: any) :: number?

--- The path of the running executable (`nil` if it can't be determined). Under `sn run` this is `sn` itself.
process.execPath = (nil :: any) :: string?

--- The program's arguments; the same array as the `args` global.
process.args = (nil :: any) :: { string }

--[=[
	The environment variables. Assigning sets a variable, assigning `nil` removes it, and
	`for name, value in process.env do` lists them. Changes apply to this process and to child
	processes started afterwards.
]=]
process.env = (nil :: any) :: { [string]: string? }

--- The current working directory.
function process.cwd(): string
	return nil :: any
end

--- Changes the current working directory.
function process.chdir(path: string): () end

--- Seconds since the program started.
function process.uptime(): number
	return nil :: any
end

--- `rss` is the bytes of RAM the process uses (`nil` if unknown); `luauHeap` is the bytes used by this VM's heap.
function process.memoryUsage(): { rss: number?, luauHeap: number }
	return nil :: any
end

--- Ends the program immediately with `code` (default 0). Standard output is flushed.
function process.exit(code: number?): never
	return nil :: any
end

--[=[
	Runs a program, waits for it and collects its output. Arguments are passed directly, without a
	shell, unless `shell = true`.

	```luau
	local result = process.exec("git", { "status", "--short" }, { cwd = "repo" })
	if result.ok then
		print(result.stdout)
	end
	```
]=]
function process.exec(program: string, args: { string }?, options: ExecOptions?): ExecResult
	return nil :: any
end

--- Starts a program and returns immediately with a `Child`.
function process.spawn(program: string, args: { string }?, options: SpawnOptions?): Child
	return nil :: any
end

--- Sends a signal (default `"terminate"`) to a process. Returns `true`, or `nil, err`.
function process.kill(pid: number, signal: Signal?): (boolean?, string?)
	return nil :: any
end

--[=[
	Each time the signal arrives, `handler(signalName)` runs as a new task. Passing `nil` restores
	the default behavior (usually: exit). A handler doesn't keep the program alive by itself.
]=]
function process.onSignal(signal: Signal, handler: ((name: string) -> ())?): () end

return process
