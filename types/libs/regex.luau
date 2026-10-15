--[[
	Type definitions for @sonata/regex, used by the Luau Language Server.
	Written into .sonata/types by `sn init --lsp`; edits are lost when they are regenerated.
]]

--[=[
	Regular expressions with ECMAScript syntax (the flavor JavaScript uses).

	The argument order is always `(text, pattern, ...)`. `flags` is an optional string; the
	supported flag is `"i"` (case-insensitive). An invalid pattern raises. Texts are limited to
	64 KiB and patterns to 1024 bytes.
]=]
local regex = {}

export type Match = {
	--- The matched text.
	text: string,
	--- 1-based position of the first character.
	start: number,
	--- 1-based position of the last character (inclusive).
	stop: number,
	--- Capture groups; `false` for groups that didn't participate.
	groups: { string | false },
}

--- Whether the pattern matches anywhere in `text`.
function regex.test(text: string, pattern: string, flags: string?): boolean
	return nil :: any
end

--- Whether the pattern matches the whole of `text`.
function regex.matches(text: string, pattern: string, flags: string?): boolean
	return nil :: any
end

--- The first match at or after position `init` (1-based, default 1), or `nil`.
function regex.find(text: string, pattern: string, flags: string?, init: number?): Match?
	return nil :: any
end

--- Every non-overlapping match.
function regex.findAll(text: string, pattern: string, flags: string?): { Match }
	return nil :: any
end

--[=[
	Replaces matches and returns the new text and the number of replacements. A negative `limit`
	(the default) replaces all. In `repl`: `$1`...`$99` a group, `$&` the whole match, `` $` `` the
	text before, `$'` the text after, `$$` a literal `$`.
]=]
function regex.replace(text: string, pattern: string, repl: string, flags: string?, limit: number?): (string, number)
	return nil :: any, nil :: any
end

--- Splits `text` on every match. Empty matches are ignored.
function regex.split(text: string, pattern: string, flags: string?): { string }
	return nil :: any
end

--- Keeps the strings that match, or with `invert`, the ones that don't.
function regex.filter(list: { string }, pattern: string, flags: string?, invert: boolean?): { string }
	return nil :: any
end

--- Escapes every special character, so `text` matches literally.
function regex.escape(text: string): string
	return nil :: any
end

--- Whether the pattern compiles. On failure, also returns the error message.
function regex.isValid(pattern: string, flags: string?): (boolean, string?)
	return nil :: any
end

--- Ready-made patterns, all anchored to match a whole string.
regex.patterns = table.freeze({
	--- A simple e-mail address.
	email = (nil :: any) :: string,
	--- An optionally signed integer.
	integer = (nil :: any) :: string,
	--- A decimal number, with optional exponent.
	number = (nil :: any) :: string,
	--- A C-style identifier.
	identifier = (nil :: any) :: string,
	--- A UUID, any case.
	uuid = (nil :: any) :: string,
	--- A dotted IPv4 address.
	ipv4 = (nil :: any) :: string,
	--- Hex digits, optionally prefixed with 0x.
	hex = (nil :: any) :: string,
})

return regex
