--[[
	Type definitions for @sonata/datetime, used by the Luau Language Server.
	Written into .sonata/types by `sn init --lsp`; edits are lost when they are regenerated.
]]

--[=[
	Dates, times and time zones, precise to the nanosecond.

	An instant is a number of Unix seconds. Every function that produces one also returns its exact
	nanosecond part; pass it back as `nsec` to stay exact.
]=]
local datetime = {}

--[=[
	A time zone (default UTC): a fixed offset in seconds east of UTC (`19800`), an offset string
	(`"UTC"`, `"Z"`, `"+05:30"`, `"UTC+2"`), an IANA name (`"Europe/Paris"`) or a POSIX TZ string
	(`"EST5EDT,M3.2.0,M11.1.0"`).
]=]
export type Zone = string | number

export type Fields = {
	year: number,
	month: number,
	day: number,
	hour: number,
	min: number,
	sec: number,
	nsec: number,
	--- Day of the week, 1 = Sunday.
	wday: number,
	--- Day of the year, 1-based.
	yday: number,
	isoYear: number,
	isoWeek: number,
	--- Seconds east of UTC.
	offset: number,
	isdst: boolean,
	--- Zone abbreviation, e.g. `"CEST"`.
	abbr: string,
}

export type Delta = {
	years: number?,
	months: number?,
	weeks: number?,
	days: number?,
	hours: number?,
	minutes: number?,
	seconds: number?,
	nsec: number?,
}

export type Unit = "year" | "month" | "week" | "day" | "hour" | "minute" | "second"

export type Scale = "unix" | "unixms" | "unixus" | "julian" | "mjd" | "excel" | "filetime" | "dotnet" | "ntp" | "cocoa"

--- The current time: `t, nsec`.
function datetime.now(): (number, number)
	return nil :: any, nil :: any
end

--- Seconds on a monotonic clock. Only meaningful for measuring durations.
function datetime.monotonic(): number
	return nil :: any
end

--- Splits an instant into calendar fields in `zone`.
function datetime.toFields(t: number, zone: Zone?, nsec: number?): Fields
	return nil :: any
end

--[=[
	Builds an instant from calendar fields interpreted in `zone`. Missing fields default to
	1970-01-01 00:00:00; out-of-range values roll over. Returns `t, nsec`.
]=]
function datetime.fromFields(fields: { [string]: number }, zone: Zone?): (number, number)
	return nil :: any, nil :: any
end

--[=[
	Parses `text` and returns `t, nsec`, or `nil, error`. `format` is a format string or a preset;
	the default, `"iso"`, auto-detects ISO 8601, RFC 3339, RFC 2822 and HTTP dates. `zone` applies
	when the text carries no offset.
]=]
function datetime.parse(text: string, format: string?, zone: Zone?): (number?, number | string)
	return nil :: any, nil :: any
end

--[=[
	Formats an instant. `format` is a format string (`"%Y-%m-%d %H:%M"`) or a preset: `"iso"`
	(default), `"rfc2822"`, `"http"`, `"date"`, `"time"` or `"ctime"`.
]=]
function datetime.format(t: number, format: string?, zone: Zone?, nsec: number?): string
	return nil :: any
end

--[=[
	Adds a duration and returns `t, nsec`. Calendar units (years, months, weeks, days) move the
	wall-clock time in `zone`; exact units move the instant itself.
]=]
function datetime.add(t: number, delta: Delta, zone: Zone?, nsec: number?): (number, number)
	return nil :: any, nil :: any
end

--- Rounds an instant down to the start of a unit in `zone` (weeks start on Monday). Returns `t, nsec`.
function datetime.startOf(t: number, unit: Unit, zone: Zone?, nsec: number?): (number, number)
	return nil :: any, nil :: any
end

--- Converts a timestamp between scales, e.g. `convert(1700000000000, "unixms", "unix")`.
function datetime.convert(value: number, from: Scale, to: Scale): number
	return nil :: any
end

function datetime.isLeapYear(year: number): boolean
	return nil :: any
end

function datetime.daysInMonth(year: number, month: number): number
	return nil :: any
end

return datetime
