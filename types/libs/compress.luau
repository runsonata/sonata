--[[
	Type definitions for @sonata/compress, used by the Luau Language Server.
	Written into .sonata/types by `sn init --lsp`; edits are lost when they are regenerated.
]]

--[=[
	Compression and decompression (gzip, zlib, raw deflate and zstd), streaming, password protection
	and checksums. Functions raise on invalid arguments, corrupt data, and output larger than the
	allowed maximum.
]=]
local compress = {}

--- `"gzip"` (default), `"zlib"`, `"deflate"` (raw) or `"zstd"` (when built with it).
export type Format = "gzip" | "zlib" | "deflate" | "zstd"

--- A number in the format's range, or `"fast"`, `"default"` or `"best"`.
export type Level = number | "fast" | "default" | "best"

--- Compresses `data` (default: gzip at the default level). With a password, the result is also encrypted.
function compress.compress(data: string, format: Format?, level: Level?, password: string?): string
	return nil :: any
end

--[=[
	Decompresses `data`. Without `format`, it is detected from the data's magic bytes (raw deflate
	must be named). `maxSize` caps the output (default 256 MiB, up to 1 GiB).
]=]
function compress.decompress(data: string, format: Format?, maxSize: number?, password: string?): string
	return nil :: any
end

--- Whether `data` is password-protected.
function compress.isEncrypted(data: string): boolean
	return nil :: any
end

--[=[
	A streaming compressor: feed chunks to `write`, then call `finish`. Each call returns the
	output that is ready so far.
]=]
function compress.encoder(format: Format?, level: Level?): ((chunk: string) -> string, () -> string)
	return nil :: any, nil :: any
end

--- A streaming decompressor. `finish()` raises if the stream was truncated.
function compress.decoder(format: Format?, maxSize: number?): ((chunk: string) -> string, () -> string)
	return nil :: any, nil :: any
end

--- The format of `data` from its magic bytes: `"gzip"`, `"zlib"`, `"zstd"`, or `nil`.
function compress.detect(data: string): Format?
	return nil :: any
end

--- The formats available in this build.
function compress.formats(): { Format }
	return nil :: any
end

--- The minimum, maximum and default level of a format (default gzip).
function compress.levels(format: Format?): (number, number, number)
	return nil :: any, nil :: any, nil :: any
end

--- The CRC-32 of `data`. Pass a previous result as `seed` to checksum data in pieces.
function compress.crc32(data: string, seed: number?): number
	return nil :: any
end

--- The Adler-32 checksum of `data`, with the same incremental seed.
function compress.adler32(data: string, seed: number?): number
	return nil :: any
end

return compress
