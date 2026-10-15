--[[
	Type definitions for @sonata/fs, used by the Luau Language Server.
	Written into .sonata/types by `sn init --lsp`; edits are lost when they are regenerated.
]]

--[=[
	Filesystem access: files, directories, metadata, globbing, watching and locking.

	Every function raises on failure, except the `exists`, `isFile`, `isDir` and `isSymlink`
	queries, which answer `false`. Paths may use `/` everywhere; returned paths always do.
]=]
local fs = {}

export type Kind = "file" | "directory" | "symlink" | "other"

export type DirEntry = {
	name: string,
	kind: Kind,
}

export type Stat = {
	kind: Kind,
	--- Bytes; 0 for anything but regular files.
	size: number,
	--- Unix seconds.
	modified: number,
	--- Permission bits.
	mode: number,
}

--[=[
	How a file is split into chunks:

	- `nil` or `"auto"`: file size / 64, rounded up to a multiple of 4 KiB, kept between 64 KiB and 4 MiB.
	- `{ count = n }`: aim for `n` chunks. The real count can be lower, never higher.
	- `{ size = n }`: exactly `n` bytes per chunk (1 to `fs.maxChunkSize`); the last chunk may be shorter.
]=]
export type ChunkSpec = "auto" | { count: number } | { size: number }

export type ChunkPlan = {
	--- Total bytes.
	size: number,
	--- Bytes per chunk.
	chunkSize: number,
	--- Number of chunks (0 for an empty file).
	chunkCount: number,
	--- Bytes in the last chunk.
	lastChunkSize: number,
}

export type Reader = {
	--- The next chunk, or `nil` after the last one. Only `read()` moves the read position.
	read: (self: Reader) -> string?,
	--- Chunk number `index` (1-based), in any order.
	readChunk: (self: Reader, index: number) -> string,
	--- Iterator: `for index, chunk in reader:chunks() do ... end`.
	chunks: (self: Reader) -> () -> (number?, string),
	--- Makes `read()` start again at chunk 1.
	rewind: (self: Reader) -> (),
	--- The same table `fs.chunkPlan` returns.
	plan: (self: Reader) -> ChunkPlan,
	--- Releases the file now. Calling it twice is fine.
	close: (self: Reader) -> (),
}

export type WalkEntry = {
	--- Relative to the walked directory: `"sub/a.txt"`.
	path: string,
	--- The walked directory joined with `path`: `"src/sub/a.txt"`.
	fullPath: string,
	--- `"a.txt"`
	name: string,
	kind: Kind,
	--- 1 for entries directly inside the walked directory.
	depth: number,
}

export type WalkOptions = {
	--- Default: unlimited (at least 1).
	maxDepth: number?,
	--- Default `false`: links are reported, not entered.
	followSymlinks: boolean?,
	--- Returning `false` skips the entry (and a directory's contents).
	filter: ((entry: WalkEntry) -> boolean)?,
	--- Default `true`: report directories too.
	dirs: boolean?,
}

export type GlobOptions = {
	--- Where relative patterns start (default `"."`).
	cwd: string?,
	--- Let wildcards match names starting with `.` (default `false`).
	dot: boolean?,
	--- Only return entries of this kind.
	kind: ("file" | "directory")?,
	--- Return absolute paths (default `false`: as the pattern wrote them).
	absolute: boolean?,
}

export type WatchOptions = {
	--- Include subdirectories (default `true`).
	recursive: boolean?,
	--- Seconds between checks (default 0.25).
	interval: number?,
}

export type WatchEvent = {
	kind: "create" | "modify" | "remove",
	path: string,
	isDir: boolean,
}

export type Watcher = {
	--- Waits for the next change. `nil` on timeout, or once the watcher is closed. Other tasks run while it waits.
	next: (self: Watcher, timeout: number?) -> WatchEvent?,
	--- The changes since the last look, without waiting.
	poll: (self: Watcher) -> { WatchEvent },
	--- Stops watching.
	close: (self: Watcher) -> (),
}

export type Lock = {
	--- Releases the lock.
	unlock: (self: Lock) -> (),
	--- Whether this lock is still held.
	isLocked: (self: Lock) -> boolean,
}

--- The largest file `readFile` accepts, in bytes (1 GiB - 1, the Luau string limit).
fs.maxReadSize = (nil :: any) :: number

--- The largest chunk a reader can produce, in bytes.
fs.maxChunkSize = (nil :: any) :: number

--- Permission presets for `fs.chmod`.
fs.modes = table.freeze({
	--- 0644
	file = 420,
	--- 0755
	executable = 493,
	--- 0755
	directory = 493,
	--- 0600
	private = 384,
	--- 0700
	privateDirectory = 448,
	--- 0444
	readOnly = 292,
})

--[=[
	Returns the whole contents of a file. Files larger than `fs.maxReadSize` are refused; use
	`fs.openReader` for those.
]=]
function fs.readFile(path: string): string
	return nil :: any
end

--- Writes `contents` to a file, creating it or truncating it first.
function fs.writeFile(path: string, contents: string): () end

--- Appends to a file, creating it if it doesn't exist.
function fs.appendFile(path: string, contents: string): () end

--- Copies a file. `overwrite` defaults to `true`; with `false`, copying onto an existing file is an error.
function fs.copyFile(from: string, to: string, overwrite: boolean?): () end

--- Deletes a file. Read-only files are deleted on Windows too, as on POSIX.
function fs.removeFile(path: string): () end

--- Renames a file or directory. Across devices, it copies and then deletes.
function fs.move(from: string, to: string): () end

--- Works out how a file (or a size in bytes, without touching any file) would be split into chunks.
function fs.chunkPlan(sizeOrPath: number | string, spec: ChunkSpec?): ChunkPlan
	return nil :: any
end

--[=[
	Opens a regular file for chunked reading. The plan is fixed when the reader opens. A reader
	keeps its file open until `close()` or until it is garbage collected.

	```luau
	local reader = fs.openReader("big.log", { size = 1024 * 1024 })
	for index, chunk in reader:chunks() do
		process(chunk)
	end
	reader:close()
	```
]=]
function fs.openReader(path: string, spec: ChunkSpec?): Reader
	return nil :: any
end

--[=[
	Creates a directory. With `recursive`, missing parents are created and an existing directory
	is fine; without it, the directory must not exist yet.
]=]
function fs.makeDir(path: string, recursive: boolean?): () end

--- Deletes a directory. Without `recursive`, it must be empty.
function fs.removeDir(path: string, recursive: boolean?): () end

--[=[
	The names of the entries in a directory, sorted. With `recursive`, every entry below it, as
	paths relative to `path` with `/` separators (`"sub/a.txt"`). Symlinks are listed, not followed.
]=]
function fs.listDir(path: string, recursive: boolean?): { string }
	return nil :: any
end

--- Like `listDir`, but each entry is a table with its name and its kind.
function fs.readDir(path: string, recursive: boolean?): { DirEntry }
	return nil :: any
end

--- Copies a directory recursively, merging into `to`. `overwrite` (default `true`) decides what happens to files that already exist.
function fs.copyDir(from: string, to: string, overwrite: boolean?): () end

--- Whether something exists at `path`. Follows symlinks.
function fs.exists(path: string): boolean
	return nil :: any
end

--- Whether `path` is a regular file. Follows symlinks.
function fs.isFile(path: string): boolean
	return nil :: any
end

--- Whether `path` is a directory. Follows symlinks.
function fs.isDir(path: string): boolean
	return nil :: any
end

--- Whether `path` is a symbolic link.
function fs.isSymlink(path: string): boolean
	return nil :: any
end

--- Information about a file. Follows symlinks.
function fs.stat(path: string): Stat
	return nil :: any
end

--- Like `stat`, but describes a symbolic link itself.
function fs.lstat(path: string): Stat
	return nil :: any
end

--- The absolute path with every symlink resolved. The path must exist.
function fs.realPath(path: string): string
	return nil :: any
end

--- Creates a symbolic link at `link` pointing to `target`.
function fs.symlink(target: string, link: string): () end

--- The target of a symbolic link.
function fs.readLink(path: string): string
	return nil :: any
end

--[=[
	Sets permission bits, e.g. `fs.chmod(p, fs.modes.executable)`.

	On Windows only the owner-write bit (0200) is looked at: set means writable, clear means read-only.
]=]
function fs.chmod(path: string, mode: number): () end

--- The current working directory.
function fs.cwd(): string
	return nil :: any
end

--- Changes the current working directory.
function fs.chdir(path: string): () end

--- The system's temporary directory.
function fs.tempDir(): string
	return nil :: any
end

--[=[
	Creates a new, empty, private directory inside the temporary directory and returns its path.
	`prefix` defaults to `"sonata-"`.
]=]
function fs.makeTempDir(prefix: string?): string
	return nil :: any
end

--[=[
	An iterator over everything below `dir`, depth first: a directory comes before its contents,
	and names are sorted within each directory.

	```luau
	for entry in fs.walk("src") do
		if entry.kind == "file" then
			print(entry.path)
		end
	end
	```
]=]
function fs.walk(dir: string, options: WalkOptions?): () -> WalkEntry?
	return nil :: any
end

--[=[
	Returns the paths matching `pattern`, sorted.

	`*` any characters except `/`, `?` one character, `[abc]` / `[a-z]` / `[!abc]` sets, `**` any
	number of directories, `{a,b}` alternatives, `\` escapes. Names starting with `.` only match
	patterns that spell out the dot, unless `dot = true`.
]=]
function fs.glob(pattern: string, options: GlobOptions?): { string }
	return nil :: any
end

--- Tests a path against a glob pattern with the same rules as `fs.glob`, without touching the disk.
function fs.match(pattern: string, path: string, options: { dot: boolean? }?): boolean
	return nil :: any
end

--[=[
	Watches a file, or a directory and what is in it. Changes are found by comparing sizes and
	modification times every `interval`.
]=]
function fs.watch(path: string, options: WatchOptions?): Watcher
	return nil :: any
end

--[=[
	Takes an advisory lock on `path`, waiting until it is acquired, or returns `nil` once `timeout`
	seconds have passed (default: wait forever). `shared = true` takes a shared (read) lock.
	Other tasks run while it waits.
]=]
function fs.lock(path: string, options: { shared: boolean?, timeout: number? }?): Lock?
	return nil :: any
end

--- Takes the lock if it is free, or returns `nil` immediately if someone else holds it.
function fs.tryLock(path: string, options: { shared: boolean? }?): Lock?
	return nil :: any
end

return fs
