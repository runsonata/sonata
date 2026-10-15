--[[
	Type definitions for @sonata/json, used by the Luau Language Server.
	Written into .sonata/types by `sn init --lsp`; edits are lost when they are regenerated.
]]

--[=[
	Parse, serialize and edit JSON. Functions raise on invalid input; decode errors give the line
	and column.

	JSON `null` is the sentinel `json.null`. A table encodes as an array if its keys are exactly
	1..n, and as an object otherwise; tag it with `json.array` or `json.object` to force one.
]=]
local json = {}

--- A path: `"a.b[2].c"` (dot-separated keys, 1-based `[index]`), or `{ "a", "b", 2, "c" }`.
export type Path = string | { string | number }

export type EncodeOptions = {
	--- A number of spaces, or a string such as `"\t"`, for pretty printing. Compact without it.
	indent: (number | string)?,
}

--- A unique value standing for JSON `null`.
json.null = (nil :: any) :: unknown

--- Parses a JSON document.
function json.decode(text: string): any
	return nil :: any
end

--- Serializes a value. Object keys are sorted, so the output is deterministic.
function json.encode(value: any, options: EncodeOptions?): string
	return nil :: any
end

--- Reads the value at `path`, or returns `default` (`nil`) if any part of the path is missing.
function json.get(value: any, path: Path, default: any?): any
	return nil :: any
end

--[=[
	Writes `value` at `path` in place, creating missing objects and arrays on the way, and returns
	`root`. An array index must name an existing element or the next free one (to append).
]=]
function json.set<T>(root: T, path: Path, value: any): T
	return nil :: any
end

--- Removes the value at `path` in place and returns it. Removing from an array shifts the following elements down.
function json.delete(root: { [any]: any }, path: Path): any
	return nil :: any
end

--[=[
	Applies a JSON Merge Patch (RFC 7396) to `target` in place and returns it. `json.null` in the
	patch deletes a key.
]=]
function json.merge<T>(target: T, patch: { [any]: any }): T
	return nil :: any
end

--- A deep copy, keeping array/object tags.
function json.clone<T>(value: T): T
	return nil :: any
end

--- Tags a table (a new empty one if `t` is omitted) so it always encodes as an array (`[]`).
function json.array<T>(t: T?): T
	return nil :: any
end

--- Tags a table (a new empty one if `t` is omitted) so it always encodes as an object (`{}`).
function json.object<T>(t: T?): T
	return nil :: any
end

return json
