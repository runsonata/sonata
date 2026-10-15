--[[
	Type definitions for @sonata/term, used by the Luau Language Server.
	Written into .sonata/types by `sn init --lsp`; edits are lost when they are regenerated.
]]

--[=[
	The terminal and the standard streams: reading and writing, colors and styles, cursor control
	and keyboard input.

	I/O failures never raise: methods that can fail return `nil, message` (`nil` alone means end of
	input). Wrong arguments, and using a stream in the wrong direction, still raise.
]=]
local term = {}

--[=[
	`term.stdin`, `term.stdout` or `term.stderr`. Write methods only work on stdout and stderr;
	read methods only on stdin.
]=]
export type Stream = {
	--- Writes the values (strings and numbers only). Returns the stream, so calls chain.
	write: (self: Stream, ...(string | number)) -> (Stream, string?),
	--- Like `write`, then a newline.
	writeLine: (self: Stream, ...(string | number)) -> (Stream, string?),
	--- Flushes buffered output.
	flush: (self: Stream) -> (Stream, string?),
	--- The next line, without its ending unless `keepEnding` is true. `nil` at end of input.
	readLine: (self: Stream, keepEnding: boolean?) -> (string?, string?),
	--- Exactly `count` bytes (fewer only at end of input). `nil` at end of input.
	read: (self: Stream, count: number) -> (string?, string?),
	--- Up to `maxCount` bytes, as soon as at least one is available. `nil` at end of input.
	readSome: (self: Stream, maxCount: number) -> (string?, string?),
	--- Everything until end of input (`""` if already there).
	readAll: (self: Stream) -> (string?, string?),
	--- An iterator over lines. It can't let other tasks run while it waits.
	lines: (self: Stream, keepEnding: boolean?) -> () -> string?,
	--- Whether the stream is a terminal.
	isTerminal: (self: Stream) -> boolean,
	--- Columns and rows, or `nil` if it isn't a terminal.
	size: (self: Stream) -> (number?, number?),
}

export type Key = {
	--- The character itself (`"a"`, `"A"`, `"é"`) or one of `"enter"`, `"tab"`, `"backspace"`, `"escape"`,
	--- `"space"`, `"up"`, `"down"`, `"left"`, `"right"`, `"home"`, `"end"`, `"pageUp"`, `"pageDown"`,
	--- `"insert"`, `"delete"`, `"f1"` ... `"f12"`.
	name: string,
	--- Set for printable keys.
	char: string?,
	ctrl: boolean,
	alt: boolean,
	shift: boolean,
	--- The raw bytes.
	sequence: string,
}

export type ClearMode = "screen" | "below" | "above" | "line" | "lineRight" | "lineLeft"

term.stdin = (nil :: any) :: Stream
term.stdout = (nil :: any) :: Stream
term.stderr = (nil :: any) :: Stream

--- Writes `message` to stdout, flushes, then reads a line from stdin. `nil` at end of input.
function term.prompt(message: string): (string?, string?)
	return nil :: any
end

--[=[
	Like `prompt`, without echoing what is typed. If `mask` is given (e.g. `"*"`), it is shown for
	each character. Returns `nil` at end of input or on Ctrl+C.
]=]
function term.password(message: string, mask: string?): string?
	return nil :: any
end

--- The terminal's columns and rows, or `nil` if there is no terminal.
function term.size(): (number?, number?)
	return nil :: any
end

--[=[
	Wraps `text` in the given styles, at the color level of stdout.

	Modifiers: `"bold"` `"dim"` `"italic"` `"underline"` `"blink"` `"inverse"` `"hidden"` `"strikethrough"`.
	Colors: `"black"` `"red"` `"green"` `"yellow"` `"blue"` `"magenta"` `"cyan"` `"white"` `"gray"`,
	`"brightRed"` ... `"brightWhite"`, and `"bg"` + any color. Exact colors: `"#ff8800"`, `"bg:#ff8800"`,
	`"fg:208"`, `"bg:208"`.
]=]
function term.style(text: string, ...: string): string
	return nil :: any
end

--- The color support of a stream (default stdout): 0 none, 1 16 colors, 2 256 colors, 3 true color.
function term.colorLevel(stream: Stream?): number
	return nil :: any
end

--- Forces a color level for every stream. `nil` goes back to detecting it.
function term.setColorLevel(level: number?): () end

--- Removes escape sequences.
function term.strip(text: string): string
	return nil :: any
end

--- The number of columns `text` takes on screen. Escape sequences take none; wide characters take 2.
function term.width(text: string): number
	return nil :: any
end

--- Moves the cursor (1-based).
function term.moveTo(column: number, row: number): () end

--- Moves the cursor relative to where it is.
function term.move(dx: number, dy: number): () end

function term.hideCursor(): () end

function term.showCursor(): () end

function term.saveCursor(): () end

function term.restoreCursor(): () end

--- Clears part of the screen: `"screen"` (default: clears and moves home), `"below"`, `"above"`, `"line"`, `"lineRight"`, `"lineLeft"`.
function term.clear(what: ClearMode?): () end

--- Switches to or from the alternate screen.
function term.altScreen(on: boolean): () end

--- Sets the window title.
function term.setTitle(text: string): () end

--- Rings the bell.
function term.bell(): () end

--- Asks the terminal where the cursor is: column, row. `nil` if it doesn't answer.
function term.cursorPosition(): (number?, number?)
	return nil :: any
end

--- The escape sequences used by the cursor and screen functions, as strings.
term.ansi = {
	moveTo = function(column: number, row: number): string
		return nil :: any
	end,
	move = function(dx: number, dy: number): string
		return nil :: any
	end,
	clear = function(what: ClearMode?): string
		return nil :: any
	end,
	--- Just the opening codes for the given styles.
	style = function(...: string): string
		return nil :: any
	end,
	hideCursor = (nil :: any) :: string,
	showCursor = (nil :: any) :: string,
	saveCursor = (nil :: any) :: string,
	restoreCursor = (nil :: any) :: string,
	altScreenOn = (nil :: any) :: string,
	altScreenOff = (nil :: any) :: string,
	reset = (nil :: any) :: string,
}

--[=[
	Turns raw mode on or off and returns the previous state. In raw mode there is no line editing
	and no echo, and Ctrl+C arrives as a key instead of a signal.
]=]
function term.rawMode(on: boolean): boolean
	return nil :: any
end

function term.isRaw(): boolean
	return nil :: any
end

--[=[
	Waits for one key press, switching to raw mode for the duration if it isn't on already.
	Returns `nil` on timeout or end of input.
]=]
function term.readKey(timeout: number?): Key?
	return nil :: any
end

return term
