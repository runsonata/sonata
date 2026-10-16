#pragma once

struct lua_State;

// Openers for the built-in libraries. Private to src/core: the outside world
// only ever sees them through LibraryRegistry / require("@sonata/<n>").
namespace sonata::lib::libs {

//void openExample(lua_State* L); // @sonata/example
void openFs(lua_State* L);       // @sonata/fs
void openPath(lua_State* L);     // @sonata/path
void openSys(lua_State* L);      // @sonata/sys
void openTask(lua_State* L);     // @sonata/task
void openProcess(lua_State* L);  // @sonata/process
void openTerm(lua_State* L);     // @sonata/term
void openNet(lua_State* L);      // @sonata/net
void openJson(lua_State* L);     // @sonata/json
void openCrypto(lua_State* L);   // @sonata/crypto
void openDatetime(lua_State* L); // @sonata/datetime
void openCompress(lua_State* L); // @sonata/compress
void openRegex(lua_State* L);    // @sonata/regex
void openSerde(lua_State* L);    // @sonata/serde
void openSonata(lua_State* L);   // @sonata/sonata
void openPocket(lua_State* L);   // @sonata/pocket
void openLuau(lua_State* L);     // @sonata/luau

} // namespace sonata::lib::libs
