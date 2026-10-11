#include <cstddef>
#include <memory>
#include <utility>

#include <sonata/core/info.hpp>
#include <sonata/core/luau/environment.hpp>
#include <sonata/core/luau/vm.hpp>
#include <sonata/core/runtime.hpp>
#include "lua.h"
#include "lualib.h"

namespace sonata::runtime {

namespace {

luau::EnvNamespace makeSonataNamespace(const ProgramInfo& info) {
    luau::EnvNamespace projectInfo;
    projectInfo.setString("name", info.name);
    projectInfo.setString("version", info.version);
    projectInfo.setString("root", info.root);

    luau::EnvNamespace sonata;
    sonata.setString("version", info::version_string());
    sonata.setString("platform", info::platform_string());
    sonata.setString("entry", info.entry);
    sonata.setBoolean("bundled", info.bundled);
    if (info.bundled) {
        sonata.setString("executable", info.executable);
    }
    sonata.set("project", std::make_shared<luau::EnvNamespace>(std::move(projectInfo)));
    return sonata;
}

// Where @sonata/process looks for "process.args".
constexpr const char* kProcessArgsKey = "sonata.process.args";

/*
 * The "args" global: an array of the program's arguments. Set before
 * luaL_sandbox(), which makes it read-only. The same list is what
 * @sonata/process exposes as "process.args".
 *
 * It's built with the Lua API rather than through Environment because
 * Environment keys everything by name, so it can't express a real array.
 */
void setScriptArguments(lua_State* L, const std::vector<std::string>& args) {
    lua_createtable(L, static_cast<int>(args.size()), 0);
    for (std::size_t i = 0; i < args.size(); ++i) {
        lua_pushlstring(L, args[i].data(), args[i].size());
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    lua_setreadonly(L, -1, 1);
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, kProcessArgsKey);
    lua_setfield(L, LUA_GLOBALSINDEX, "args");
}

} // namespace

void runProgram(luau::ModuleLoader& loader, const ProgramInfo& info,
                const std::vector<std::string>& args, const Pocket* pocket) {
    luau::VM vm;

    luau::Environment environment;
    environment.setNamespace("sonata", makeSonataNamespace(info));
    environment.load(vm);
    setScriptArguments(vm.state(), args);

    Pocket::install(vm.state(), pocket);

    // require() has to be defined before the globals are made read-only.
    loader.install(vm);
    luaL_sandbox(vm.state());

    loader.run(vm, info.entry);
}

} // namespace sonata::runtime
