--[[
	Type definitions for @sonata/sonata, used by the Luau Language Server.
	Written into .sonata/types by `sn init --lsp`; edits are lost when they are regenerated.
]]

--[=[
	Access to Sonata's own internals: build information, projects and the package manager.

	This is not the `sonata` global: the global describes the running program, this library the
	Sonata installation. Failures raise. Where a project path is optional, the default is `"."`.
]=]
local sonata = {}

export type Info = {
	--- `"Sonata"`
	name: string,
	--- `"sonata"`
	id: string,
	--- `"sn"`
	binary: string,
	tagline: string,
	--- `"0.1.8"` or `"0.2.0-alpha.1"`
	version: string,
	semver: { major: number, minor: number, patch: number, pre: string },
	--- Short commit hash, `""` if unknown.
	gitHash: string,
	--- `""` if unknown.
	luauVersion: string,
	--- `"release"`, `"debug"`, ...
	buildType: string,
	debug: boolean,
	--- `"linux-x64"`, `"macos-arm64"`, `"windows-x64"`, ...
	platform: string,
	--- `"gcc 13.2.0"`, `"clang 17.0.6"`, `"msvc 1939"`
	compiler: string,
}

export type Dependency = {
	name: string,
	url: string,
	--- Optional when passed in; `""` when returned without one.
	ref: string?,
}

export type Package = {
	name: string,
	url: string,
	ref: string,
	--- Where it is installed (`.sonata/deps/<name>`).
	path: string,
	--- The exact commit, `""` if unknown.
	commit: string,
	--- `true` if this call fetched it.
	fresh: boolean,
}

export type Manifest = {
	name: string,
	version: string,
	description: string,
	--- As written in the manifest.
	entrypoint: string,
	authors: { string },
	dependencies: { Dependency },
}

--- A snapshot of a project: changing it changes nothing on disk.
export type Project = {
	root: string,
	--- Absolute path of the entry script.
	entrypoint: string,
	--- `<root>/.sonata`
	sonataDir: string,
	--- `<root>/.sonata/project.luau`
	projectFile: string,
	--- `<root>/.sonata/deps`
	dependenciesDir: string,
	hasDependenciesDirectory: boolean,
	--- Every .luau file of the project (outside .sonata).
	sources: { string },
	manifest: Manifest,
}

--- Read-only information about the Sonata build running the program.
sonata.info = (nil :: any) :: Info

--- Opens the project whose root is `dir`. Raises if `dir` isn't a valid project root.
function sonata.openProject(dir: string?): Project
	return nil :: any
end

--- Finds the project containing `start` (a folder or a file), searching its parent folders.
function sonata.findProject(start: string?): Project
	return nil :: any
end

--- Whether `dir` is a valid project root, and if not, why.
function sonata.validateProject(dir: string?): (boolean, string?)
	return nil :: any
end

--- Whether a dependency declaration follows the rules, and if not, the first rule it breaks.
function sonata.validateDependency(dep: Dependency): (boolean, string?)
	return nil :: any
end

--[=[
	Makes sure every dependency in the manifest (and theirs) is installed. Returns the whole set in
	install order, and the package manager's progress messages.
]=]
function sonata.install(project: string?): ({ Package }, { string })
	return nil :: any, nil :: any
end

--- Re-fetches every package from its ref, ignoring the lock file, or only `name`.
function sonata.update(name: string?, project: string?): ({ Package }, { string })
	return nil :: any, nil :: any
end

--- Adds (or replaces) a dependency in the manifest and installs it. If installing fails, the manifest is restored.
function sonata.add(dep: Dependency, project: string?): (Package, { string })
	return nil :: any, nil :: any
end

--- Removes a dependency from the manifest and deletes every folder nothing needs any more. Returns the deleted names.
function sonata.remove(name: string, project: string?): ({ string }, { string })
	return nil :: any, nil :: any
end

--- Deletes every installed package that nothing depends on. Returns the deleted names.
function sonata.prune(project: string?): ({ string }, { string })
	return nil :: any, nil :: any
end

--- Everything currently in `.sonata/deps`, sorted by name.
function sonata.installed(project: string?): ({ Package }, { string })
	return nil :: any, nil :: any
end

return sonata
