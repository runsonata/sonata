--[[
	Type definitions for @sonata/serde, used by the Luau Language Server.
	Written into .sonata/types by `sn init --lsp`; edits are lost when they are regenerated.
]]

--[=[
	Parse, serialize and convert structured text (YAML, TOML, INI, CSV, TSV), plus a toolkit for
	writing your own formats. Functions raise on invalid input, with the line and column.
]=]
local serde = {}

export type Options = {
	--- yaml encode: indentation, 2 to 8 (default 2).
	indent: number?,
	--- encode: sort table keys (default `true`).
	sort: boolean?,
	--- decode: leave out null entries of tables.
	dropNull: boolean?,
	--- csv, tsv: the first row is a header (default `true`).
	header: boolean?,
	--- ini, csv, tsv: turn numbers and booleans into values instead of strings.
	typed: boolean?,
	--- csv: the field separator, one character (default `","`).
	delimiter: string?,
	[string]: any,
}

export type Format = {
	decode: ((text: string, options: Options?) -> any)?,
	encode: ((value: any, options: Options?) -> string)?,
}

--- A built-in or registered format name (`"yaml"`, `"yml"`, `"toml"`, `"ini"`, `"csv"`, `"tsv"`), or a format table.
export type FormatSpec = string | Format

export type Scanner = {
	--- The text being scanned.
	text: string,
	--- The current position, 0-based. Assign to it to backtrack.
	pos: number,
	--- Whether the end has been reached.
	eof: (self: Scanner) -> boolean,
	--- The next `n` characters (default 1), without consuming them.
	peek: (self: Scanner, n: number?) -> string,
	--- Consumes and returns the next `n` characters (default 1).
	next: (self: Scanner, n: number?) -> string,
	--- Consumes `literal` if the text continues with it.
	eat: (self: Scanner, literal: string) -> boolean,
	--- Like `eat`, but raises (with the position) if the literal isn't next.
	expect: (self: Scanner, literal: string) -> string,
	--- Consumes characters while they are in `set` (literals, ranges like `a-z`, `%s %d %a %w %x`, leading `^` negates).
	take: (self: Scanner, set: string) -> string,
	--- The rest of the current line, without its newline; moves to the start of the next line.
	line: (self: Scanner) -> string,
	--- Everything not consumed yet. Doesn't move.
	rest: (self: Scanner) -> string,
	--- The current line and column, both 1-based.
	where: (self: Scanner) -> (number, number),
	--- Raises `"message (line L, column C)"`.
	fail: (self: Scanner, message: string) -> never,
}

export type Writer = {
	--- Appends strings, numbers and booleans as they are.
	write: (self: Writer, ...(string | number | boolean)) -> Writer,
	--- Writes the current indentation, the values, then a newline.
	line: (self: Writer, ...(string | number | boolean)) -> Writer,
	--- Indents following lines by `n` more levels (default 1).
	indent: (self: Writer, n: number?) -> Writer,
	--- Indents following lines by `n` fewer levels (default 1).
	dedent: (self: Writer, n: number?) -> Writer,
	--- The text so far. `tostring(w)` works too.
	str: (self: Writer) -> string,
}

--- A unique value standing for null.
serde.null = (nil :: any) :: unknown

--- Parses `text`.
function serde.decode(format: FormatSpec, text: string, options: Options?): any
	return nil :: any
end

--- Serializes `value`.
function serde.encode(format: FormatSpec, value: any, options: Options?): string
	return nil :: any
end

--- Decodes and re-encodes in one call.
function serde.convert(from: FormatSpec, to: FormatSpec, text: string, options: Options?): string
	return nil :: any
end

--- The names of every available format, built-in and registered.
function serde.formats(): { string }
	return nil :: any
end

--- Makes a format available by name. Registered names take precedence over built-in ones. `nil` unregisters it.
function serde.register(name: string, format: Format?): () end

--- A cursor over `text`, for writing parsers.
function serde.scanner(text: string): Scanner
	return nil :: any
end

--- A string builder with indentation, for writing encoders. Every method returns the writer.
function serde.writer(indent: (string | number)?): Writer
	return nil :: any
end

--- Escapes a string the way most formats do (`\n`, `\"`, ...).
function serde.escape(text: string, quote: string?): string
	return nil :: any
end

--- Reverses `escape`.
function serde.unescape(text: string): string
	return nil :: any
end

--- The shortest text that reads back as the same number.
function serde.formatNumber(n: number): string
	return nil :: any
end

--- `true` for `{}` and for tables whose keys are exactly 1..n.
function serde.isArray(t: { [any]: any }): boolean
	return nil :: any
end

return serde
