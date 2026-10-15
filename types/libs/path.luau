--[[
	Type definitions for @sonata/path, used by the Luau Language Server.
	Written into .sonata/types by `sn init --lsp`; edits are lost when they are regenerated.
]]

--[=[
	Lexical path manipulation. Nothing here touches the disk except `resolve`, which reads the
	current directory.

	`path.*` uses the style of the host OS. `path.posix.*` and `path.win32.*` are the same functions
	for one fixed style. Functions raise on bad arguments.
]=]
local path = {}

export type Style = "posix" | "win32"

export type ParsedPath = {
	root: string,
	dir: string,
	base: string,
	ext: string,
	name: string,
}

export type PathParts = {
	root: string?,
	dir: string?,
	base: string?,
	ext: string?,
	name: string?,
}

--- The separator: `"/"`, or `"\\"` for win32.
path.sep = (nil :: any) :: string

--- The PATH list delimiter: `":"`, or `";"` for win32.
path.delimiter = (nil :: any) :: string

--- The style these functions use.
path.style = (nil :: any) :: Style

--- Removes `.` segments and duplicate separators, and resolves `..`: `normalize("/a/./b//c/..")` is `"/a/b"`.
function path.normalize(p: string): string
	return nil :: any
end

--- Joins segments and normalizes the result. With no arguments, returns `"."`.
function path.join(...: string): string
	return nil :: any
end

--[=[
	Processes the segments from right to left until an absolute path is formed, prepending the
	current directory if none is.
]=]
function path.resolve(...: string): string
	return nil :: any
end

--- The path from `from` to `to`. Returns `"."` for the same path.
function path.relative(from: string, to: string): string
	return nil :: any
end

--- Whether `child` is `parent` or somewhere below it.
function path.isInside(child: string, parent: string): boolean
	return nil :: any
end

--- Everything but the last segment: `dirname("/a/b/c.txt")` is `"/a/b"`, `dirname("a")` is `"."`.
function path.dirname(p: string): string
	return nil :: any
end

--- The last segment, with `suffix` removed if it ends with it.
function path.basename(p: string, suffix: string?): string
	return nil :: any
end

--- The extension, with its dot: `extname("c.tar.gz")` is `".gz"`.
function path.extname(p: string): string
	return nil :: any
end

--- The base name without its extension: `stem("/a/b/c.tar.gz")` is `"c.tar"`.
function path.stem(p: string): string
	return nil :: any
end

--- Replaces the extension. An empty `ext` removes it.
function path.withExtension(p: string, ext: string): string
	return nil :: any
end

function path.isAbsolute(p: string): boolean
	return nil :: any
end

--- The segments of a path: `components("/a/b/../c")` is `{ "/", "a", "b", "..", "c" }`.
function path.components(p: string): { string }
	return nil :: any
end

--- Splits a path into its parts.
function path.parse(p: string): ParsedPath
	return nil :: any
end

--- The reverse of `parse`.
function path.format(parts: PathParts): string
	return nil :: any
end

--- Converts separators to `/`. Only the win32 style changes anything.
function path.toPosix(p: string): string
	return nil :: any
end

--- Converts separators to the style's own separator. Only the win32 style changes anything.
function path.toNative(p: string): string
	return nil :: any
end

--- Splits a PATH-style list on `path.delimiter`. Empty entries are dropped.
function path.splitList(list: string): { string }
	return nil :: any
end

--- Joins entries into a PATH-style list.
function path.joinList(entries: { string }): string
	return nil :: any
end

export type Path = typeof(path)

--- The same functions, always with POSIX style.
path.posix = path :: Path

--- The same functions, always with Windows style. Comparisons are ASCII case-insensitive.
path.win32 = path :: Path

return path
