#include <algorithm>
#include <stdexcept>

#include <sonata/core/library.hpp>
#include "lua.h"

namespace sonata::lib {

namespace {

bool isValidLibraryName(std::string_view name) {
    if (name.empty()) {
        return false;
    }

    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

} // namespace

void setFunctions(lua_State* L, const NativeFunction* functions, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        lua_pushcclosurek(L, functions[i].function, functions[i].name, 0, functions[i].continuation);
        lua_setfield(L, -2, functions[i].name);
    }
}

void LibraryRegistry::add(NativeLibrary library) {
    if (!isValidLibraryName(library.name)) {
        throw std::invalid_argument(
            "invalid library name '" + library.name + "' (use lowercase letters, digits, '_' and '-')"
        );
    }

    if (library.open == nullptr) {
        throw std::invalid_argument("library '" + library.name + "' has no opener");
    }

    if (libraries_.find(library.name) != libraries_.end()) {
        throw std::invalid_argument("library '" + library.name + "' is already registered");
    }

    std::string name = library.name;
    libraries_.emplace(std::move(name), std::move(library));
}

const NativeLibrary* LibraryRegistry::find(std::string_view name) const {
    const auto it = libraries_.find(name);
    return it == libraries_.end() ? nullptr : &it->second;
}

std::vector<std::string> LibraryRegistry::names() const {
    std::vector<std::string> result;
    result.reserve(libraries_.size());

    for (const auto& entry : libraries_) {
        result.push_back(entry.first);
    }

    return result; // std::map is already sorted
}

} // namespace sonata::lib
