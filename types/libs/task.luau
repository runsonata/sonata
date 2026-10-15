--[[
	Type definitions for @sonata/task, used by the Luau Language Server.
	Written into .sonata/types by `sn init --lsp`; edits are lost when they are regenerated.
]]

--[=[
	Tasks (lightweight threads) and the tools to coordinate them.

	Only one task runs at a time; a task gives the others a turn whenever it waits. Wherever a
	function is expected, a suspended coroutine works too.
]=]
local task = {}

export type Status = "running" | "normal" | "suspended" | "waiting" | "scheduled" | "done" | "error" | "cancelled"

export type Channel = {
	--- Returns `true`, or `false, "closed"`, or `false, "timed out"`.
	send: (self: Channel, value: any, timeout: number?) -> (boolean, string?),
	--- Returns `true, value`, or `false, "closed"`, or `false, "timed out"`.
	recv: (self: Channel, timeout: number?) -> (boolean, any),
	--- Never waits.
	trySend: (self: Channel, value: any) -> boolean,
	--- Returns `true, value`, or `false`. Never waits.
	tryRecv: (self: Channel) -> (boolean, any),
	--- Wakes every waiter. Values already sent can still be received.
	close: (self: Channel) -> (),
	isClosed: (self: Channel) -> boolean,
	--- The number of values waiting to be received (same as `#ch`).
	len: (self: Channel) -> number,
	--- The capacity (`math.huge` for an unbounded channel).
	capacity: (self: Channel) -> number,
}

export type Mutex = {
	--- Whether the lock was acquired.
	lock: (self: Mutex, timeout: number?) -> boolean,
	--- Never waits.
	tryLock: (self: Mutex) -> boolean,
	--- Only the thread that locked it may unlock it.
	unlock: (self: Mutex) -> (),
	isLocked: (self: Mutex) -> boolean,
}

export type Semaphore = {
	acquire: (self: Semaphore, timeout: number?) -> boolean,
	--- Never waits.
	tryAcquire: (self: Semaphore) -> boolean,
	release: (self: Semaphore) -> (),
	--- The number of free permits.
	available: (self: Semaphore) -> number,
}

export type Event = {
	--- `true` once the event is set, `false` on timeout.
	wait: (self: Event, timeout: number?) -> boolean,
	--- Sets the flag and wakes every waiter.
	set: (self: Event) -> (),
	--- Clears the flag.
	reset: (self: Event) -> (),
	isSet: (self: Event) -> boolean,
}

export type Group = {
	--- Starts a task in the group.
	spawn: <A...>(self: Group, fn: ((A...) -> ...any) | thread, A...) -> thread,
	--- `ok, { error }` once every task has finished (`ok` is false if any failed), or `nil, "timed out"`.
	wait: (self: Group, timeout: number?) -> (boolean?, any),
	--- Cancels every task in the group.
	cancel: (self: Group) -> (),
	--- The number of tasks still running.
	count: (self: Group) -> number,
}

--- Runs `fn(...)` as a new task right now, until it first waits, then returns its thread.
function task.spawn<A...>(fn: ((A...) -> ...any) | thread, ...: A...): thread
	return nil :: any
end

--- Runs `fn(...)` as a new task at the end of the current scheduler cycle.
function task.defer<A...>(fn: ((A...) -> ...any) | thread, ...: A...): thread
	return nil :: any
end

--- Runs `fn(...)` as a new task once `seconds` have passed.
function task.delay<A...>(seconds: number, fn: ((A...) -> ...any) | thread, ...: A...): thread
	return nil :: any
end

--[=[
	Suspends the current task for `seconds` (default 0: until the next cycle) and returns how long
	it really waited. Other tasks run in the meantime.
]=]
function task.wait(seconds: number?): number
	return nil :: any
end

--- Stops a task for good, wherever it is waiting.
function task.cancel(thread: thread): () end

--- The task running right now, or `nil` outside of one.
function task.running(): thread?
	return nil :: any
end

function task.status(thread: thread): Status
	return nil :: any
end

--[=[
	Waits for a task to finish. Returns `true, ...results` if it returned, `false, error` if it
	failed or was cancelled, and `nil, "timed out"` if `timeout` seconds passed first.
]=]
function task.join(thread: thread, timeout: number?): (boolean?, ...any)
	return nil :: any
end

--[=[
	Runs the functions as tasks and waits for all of them. Each entry of the result is that task's
	first result. The first error is raised, and the tasks still running are cancelled.
]=]
function task.all(tasks: { (() -> ...any) | thread }): { any }
	return nil :: any
end

--[=[
	Runs the functions as tasks; the first to finish wins and the others are cancelled. Returns the
	winner's index and results. Raises if the winner failed.
]=]
function task.race(tasks: { (() -> ...any) | thread }): (number, ...any)
	return nil :: any
end

--[=[
	Runs `fn(...)` as a task and cancels it after `seconds`. Returns `true, ...results`, or
	`false, error`; the error is `"timed out"` if time ran out.
]=]
function task.timeout<A...>(seconds: number, fn: ((A...) -> ...any) | thread, ...: A...): (boolean, ...any)
	return nil :: any
end

--- The number of tasks that are scheduled or waiting.
function task.count(): number
	return nil :: any
end

--[=[
	Sets the function called (as a new task) when a task fails and nobody joins it. `nil` restores
	the default, which prints the message and traceback to stderr.
]=]
function task.onError(handler: ((message: string, traceback: string, thread: thread) -> ())?): () end

--[=[
	A queue for passing values between tasks. With `capacity` n > 0, `send` waits while the channel
	holds n values. With 0, every send waits for a receiver. Without one, it is unbounded.
]=]
function task.channel(capacity: number?): Channel
	return nil :: any
end

--- A lock for one task at a time. Not reentrant.
function task.mutex(): Mutex
	return nil :: any
end

function task.semaphore(permits: number): Semaphore
	return nil :: any
end

--- A flag tasks can wait for.
function task.event(): Event
	return nil :: any
end

--- Tasks managed together.
function task.group(): Group
	return nil :: any
end

return task
