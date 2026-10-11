# src/CMakeLists.txt
include(GNUInstallDirs)

configure_file(${PROJECT_SOURCE_DIR}/cmake/version.hpp.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/sonata/version.hpp @ONLY)

# ---- Built-in libraries ----
# Every library Sonata knows about. Each one lives in core/libs/<name>.cpp and
# exports libs::open<Name>(). A library that is too big for one file may put
# the rest of its sources in core/libs/<name>/ (every .cpp there is compiled
# along with it).
#
# SONATA_LIBS picks which of them are compiled in. A normal build has all of
# them; "sn build" sets it when it builds a runtime tailored to one program.
# Libraries that aren't picked aren't compiled, and their native dependencies
# (OpenSSL for net, zstd/libsodium for compress) aren't even looked for.
set(SONATA_ALL_LIBS
  fs path sys task process term net json crypto datetime compress regex serde sonata pocket)
set(SONATA_LIBS "${SONATA_ALL_LIBS}" CACHE STRING
    "Built-in libraries to compile in (semicolon-separated)")

# "stdio" became part of "term". Build folders configured before that still
# have it in their cache.
if("stdio" IN_LIST SONATA_LIBS)
  list(TRANSFORM SONATA_LIBS REPLACE "^stdio$" "term")
  list(REMOVE_DUPLICATES SONATA_LIBS)
  set(SONATA_LIBS "${SONATA_LIBS}" CACHE STRING "Built-in libraries to compile in (semicolon-separated)" FORCE)
  message(STATUS "sonata: SONATA_LIBS: 'stdio' is now part of 'term'")
endif()

set(sonata_lib_sources "")
set(SONATA_LIB_DECLARATIONS "")
set(SONATA_LIB_REGISTRATIONS "")
set(SONATA_LIB_NAMES "")

foreach(lib IN LISTS SONATA_ALL_LIBS)
  string(APPEND SONATA_LIB_NAMES "    \"${lib}\",\n")
endforeach()

foreach(lib IN LISTS SONATA_LIBS)
  if(NOT lib IN_LIST SONATA_ALL_LIBS)
    message(FATAL_ERROR "SONATA_LIBS: unknown library '${lib}' (known: ${SONATA_ALL_LIBS})")
  endif()

  # "datetime" -> "openDatetime"
  string(SUBSTRING ${lib} 0 1 first)
  string(SUBSTRING ${lib} 1 -1 rest)
  string(TOUPPER ${first} first)
  set(opener "open${first}${rest}")

  list(APPEND sonata_lib_sources core/libs/${lib}.cpp)
  file(GLOB lib_extra_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/core/libs/${lib}/*.cpp)
  list(APPEND sonata_lib_sources ${lib_extra_sources})
  string(APPEND SONATA_LIB_DECLARATIONS "void ${opener}(lua_State* L);\n")
  string(APPEND SONATA_LIB_REGISTRATIONS "    registry.add({\"${lib}\", libs::${opener}});\n")
endforeach()

configure_file(${PROJECT_SOURCE_DIR}/cmake/builtin_registry.cpp.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/builtin_registry.cpp @ONLY)

set(sonata_sources
  # systems
  core/package.cpp
  core/project.cpp
  core/module.cpp
  core/library.cpp
  core/system.cpp
  core/selection.cpp
  core/pocket.cpp
  core/bundle.cpp
  core/runtime.cpp
  core/scheduler.cpp
  # luau systems
  core/luau/compiler.cpp
  core/luau/datafile.cpp
  core/luau/environment.cpp
  core/luau/vm.cpp
  # builtin libs (see SONATA_LIBS above)
  ${sonata_lib_sources}
  ${CMAKE_CURRENT_BINARY_DIR}/generated/builtin_registry.cpp
)

# ---- Third-party dependencies ----
# With SONATA_STATIC_DEPS, static copies are tried first and shared ones are the
# fallback (SONATA_STANDALONE makes the static copy mandatory).

# A cache entry left over from a configure that found the other kind of
# library (shared vs static) would stop find_package() from looking again.
function(sonata_forget_cached_libraries want_static)
  if(want_static)
    set(wrong "\\.(so|dylib)(\\.[0-9]+)*$")
  else()
    set(wrong "\\.a$")
  endif()
  foreach(var IN LISTS ARGN)
    if(DEFINED CACHE{${var}} AND "$CACHE{${var}}" MATCHES "${wrong}")
      unset(${var} CACHE)
    endif()
  endforeach()
endfunction()

# Distro static archives usually aren't position-independent, so they can't go
# into a shared libsonata.
set(sonata_static_deps ${SONATA_STATIC_DEPS})
if(BUILD_SHARED_LIBS AND SONATA_STATIC_DEPS)
  if(SONATA_STANDALONE)
    message(FATAL_ERROR "SONATA_STANDALONE can't be combined with BUILD_SHARED_LIBS.")
  endif()
  message(STATUS "sonata: BUILD_SHARED_LIBS is ON, so OpenSSL and zlib are linked dynamically")
  set(sonata_static_deps OFF)
endif()

set(sonata_want_static_zlib OFF)
if(sonata_static_deps AND UNIX AND NOT APPLE) # macOS's zlib is part of the OS
  set(sonata_want_static_zlib ON)
endif()

sonata_forget_cached_libraries(${sonata_want_static_zlib} ZLIB_LIBRARY ZLIB_LIBRARY_RELEASE ZLIB_LIBRARY_DEBUG)
if(sonata_want_static_zlib)
  set(ZLIB_USE_STATIC_LIBS ON)
  find_package(ZLIB QUIET)
  if(NOT ZLIB_FOUND)
    if(SONATA_STANDALONE)
      message(FATAL_ERROR "SONATA_STANDALONE needs a static zlib (libz.a); install it (e.g. zlib1g-dev).")
    endif()
    message(STATUS "sonata: no static zlib found, linking it dynamically")
    set(ZLIB_USE_STATIC_LIBS OFF)
    unset(ZLIB_LIBRARY CACHE)
    unset(ZLIB_LIBRARY_RELEASE CACHE)
    unset(ZLIB_LIBRARY_DEBUG CACHE)
  endif()
endif()
find_package(ZLIB REQUIRED)

set(sonata_openssl_static OFF)
if("net" IN_LIST SONATA_LIBS)
  sonata_forget_cached_libraries(${sonata_static_deps} OPENSSL_SSL_LIBRARY OPENSSL_CRYPTO_LIBRARY)
  if(sonata_static_deps)
    set(OPENSSL_USE_STATIC_LIBS TRUE)
    # MSVC: the static OpenSSL build has to match the C runtime, which is
    # only the static one (/MT) for standalone builds.
    if(MSVC AND SONATA_STANDALONE)
      set(OPENSSL_MSVC_STATIC_RT TRUE)
    endif()
    find_package(OpenSSL QUIET)

    if(OPENSSL_FOUND)
      set(sonata_openssl_static ON)
    else()
      if(SONATA_STANDALONE)
        message(FATAL_ERROR "SONATA_STANDALONE needs static OpenSSL libraries (libssl.a/libcrypto.a).")
      endif()
      message(STATUS "sonata: no static OpenSSL found, linking it dynamically")
      set(OPENSSL_USE_STATIC_LIBS FALSE)
      foreach(var OPENSSL_SSL_LIBRARY OPENSSL_CRYPTO_LIBRARY
                  LIB_EAY_DEBUG LIB_EAY_RELEASE SSL_EAY_DEBUG SSL_EAY_RELEASE
                  LIB_EAY_LIBRARY_DEBUG LIB_EAY_LIBRARY_RELEASE
                  SSL_EAY_LIBRARY_DEBUG SSL_EAY_LIBRARY_RELEASE)
        unset(${var} CACHE)
      endforeach()
    endif()
  endif()

  find_package(OpenSSL REQUIRED)
  if(sonata_openssl_static)
    message(STATUS "sonata: OpenSSL linked statically")
  endif()
endif()

set(SONATA_ZSTD_TARGET "")
set(SONATA_SODIUM_TARGET "")

if(SONATA_COMPRESS_ZSTD AND "compress" IN_LIST SONATA_LIBS)
  find_package(zstd CONFIG QUIET)
  foreach(t zstd::libzstd_static zstd::libzstd zstd::libzstd_shared)
    if(NOT SONATA_ZSTD_TARGET AND TARGET ${t})
      set(SONATA_ZSTD_TARGET ${t})
    endif()
  endforeach()

  if(NOT SONATA_ZSTD_TARGET)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
      if(sonata_static_deps)
        set(PKG_CONFIG_USE_STATIC_LIBS ON)
      endif()
      pkg_check_modules(SONATA_ZSTD_PC QUIET IMPORTED_TARGET libzstd)
      if(SONATA_ZSTD_PC_FOUND)
        set(SONATA_ZSTD_TARGET PkgConfig::SONATA_ZSTD_PC)
      endif()
    endif()
  endif()

  if(NOT SONATA_ZSTD_TARGET)
    message(FATAL_ERROR
      "SONATA_COMPRESS_ZSTD is ON but libzstd was not found. Install it "
      "(libzstd-dev / brew install zstd / vcpkg install zstd), point "
      "CMAKE_PREFIX_PATH at it, or configure with -DSONATA_COMPRESS_ZSTD=OFF.")
  endif()
  message(STATUS "sonata: compress: zstd enabled (${SONATA_ZSTD_TARGET})")
endif()

if(SONATA_COMPRESS_CRYPTO AND "compress" IN_LIST SONATA_LIBS)
  find_package(unofficial-sodium CONFIG QUIET)
  if(TARGET unofficial-sodium::sodium)
    set(SONATA_SODIUM_TARGET unofficial-sodium::sodium)
  endif()

  if(NOT SONATA_SODIUM_TARGET)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
      if(sonata_static_deps)
        set(PKG_CONFIG_USE_STATIC_LIBS ON)
      endif()
      pkg_check_modules(SONATA_SODIUM_PC QUIET IMPORTED_TARGET libsodium)
      if(SONATA_SODIUM_PC_FOUND)
        set(SONATA_SODIUM_TARGET PkgConfig::SONATA_SODIUM_PC)
      endif()
    endif()
  endif()

  if(NOT SONATA_SODIUM_TARGET)
    find_path(SONATA_SODIUM_INCLUDE_DIR sodium.h)
    find_library(SONATA_SODIUM_LIBRARY NAMES sodium libsodium)
    if(SONATA_SODIUM_INCLUDE_DIR AND SONATA_SODIUM_LIBRARY)
      add_library(sonata_sodium UNKNOWN IMPORTED)
      set_target_properties(sonata_sodium PROPERTIES
        IMPORTED_LOCATION "${SONATA_SODIUM_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${SONATA_SODIUM_INCLUDE_DIR}")
      if(WIN32 AND SONATA_SODIUM_LIBRARY MATCHES "\\.lib$" AND NOT SONATA_SODIUM_LIBRARY MATCHES "dll")
        set_property(TARGET sonata_sodium APPEND PROPERTY
          INTERFACE_COMPILE_DEFINITIONS SODIUM_STATIC)
      endif()
      set(SONATA_SODIUM_TARGET sonata_sodium)
    endif()
  endif()

  if(NOT SONATA_SODIUM_TARGET)
    message(FATAL_ERROR
      "SONATA_COMPRESS_CRYPTO is ON but libsodium was not found. Install it "
      "(libsodium-dev / brew install libsodium / vcpkg install libsodium), "
      "point CMAKE_PREFIX_PATH at it, or configure with -DSONATA_COMPRESS_CRYPTO=OFF.")
  endif()
  message(STATUS "sonata: compress: password support enabled (${SONATA_SODIUM_TARGET})")
endif()

# ---- Build metadata for sonata/core/info.hpp ----
# Version numbers come from project(... VERSION x.y.z) in the root CMakeLists.
# The root may also pre-set SONATA_VERSION_PRE, SONATA_GIT_HASH and
# SONATA_LUAU_VERSION; anything it doesn't set is filled in or left empty here.
if(NOT DEFINED SONATA_GIT_HASH)
  set(SONATA_GIT_HASH "")
  find_package(Git QUIET)
  if(GIT_FOUND)
    execute_process(
      COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
      WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
      OUTPUT_VARIABLE SONATA_GIT_HASH
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE git_result
      ERROR_QUIET)
    if(NOT git_result EQUAL 0)
      set(SONATA_GIT_HASH "")
    endif()
  endif()
endif()

set(sonata_info_defs
  "SONATA_NET_TLS"
  "SONATA_GIT_HASH=\"${SONATA_GIT_HASH}\""
  "SONATA_LUAU_VERSION=\"${SONATA_LUAU_VERSION}\""
  "SONATA_VERSION_PRE=\"${SONATA_VERSION_PRE}\""
  "SONATA_BUILD_TYPE=\"$<LOWER_CASE:$<CONFIG>>\"")

foreach(part MAJOR MINOR PATCH)
  if(NOT "${PROJECT_VERSION_${part}}" STREQUAL "")
    list(APPEND sonata_info_defs SONATA_VERSION_${part}=${PROJECT_VERSION_${part}})
  endif()
endforeach()

# ---- The library ----
function(sonata_setup_library target)
  target_compile_features(${target} PUBLIC cxx_std_20)

  target_include_directories(${target}
    PUBLIC
      $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/core)

  target_link_libraries(${target} PUBLIC Luau.Common Luau.VM Luau.Compiler Luau.Ast)
  target_link_libraries(${target} PRIVATE ZLIB::ZLIB)

  if("net" IN_LIST SONATA_LIBS)
    target_link_libraries(${target} PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    if(WIN32 AND sonata_openssl_static)
      target_link_libraries(${target} PUBLIC crypt32 ws2_32 user32 advapi32)
    endif()
    if(WIN32)
      target_link_libraries(${target} PRIVATE iphlpapi) # net.interfaces, DNS server list
    endif()
  endif()

  if(SONATA_ZSTD_TARGET)
    target_link_libraries(${target} PRIVATE ${SONATA_ZSTD_TARGET})
    target_compile_definitions(${target} PUBLIC SONATA_COMPRESS_ZSTD)
  endif()

  if(SONATA_SODIUM_TARGET)
    target_link_libraries(${target} PRIVATE ${SONATA_SODIUM_TARGET})
    target_compile_definitions(${target} PUBLIC SONATA_COMPRESS_CRYPTO)
  endif()

  if(WIN32)
    target_link_libraries(${target} PRIVATE ws2_32)
  endif()

  # PUBLIC so the CLI and installed consumers see the same values as the
  # library (info.hpp is all inline constexpr).
  target_compile_definitions(${target} PUBLIC ${sonata_info_defs})
endfunction()

add_library(sonata ${sonata_sources})
sonata_setup_library(sonata)
add_library(sonata::sonata ALIAS sonata)

# The runtime must never need libsonata.so next to it, so with
# BUILD_SHARED_LIBS it links a static copy of the library instead.
if(SONATA_BUILD_RUNTIME AND BUILD_SHARED_LIBS)
  add_library(sonata_static STATIC ${sonata_sources})
  sonata_setup_library(sonata_static)
  add_library(sonata::runtime ALIAS sonata_static)
else()
  add_library(sonata::runtime ALIAS sonata)
endif()

if(SONATA_INSTALL AND SONATA_OWNS_LUAU)
  include(CMakePackageConfigHelpers)

  set(SONATA_LUAU_TARGETS Luau.Common Luau.Ast Luau.Compiler Luau.VM Luau.Bytecode)
  set(SONATA_LUAU_INCLUDE_DEST ${CMAKE_INSTALL_INCLUDEDIR}/sonata/luau)

  # Make Luau's include paths exportable
  foreach(t IN LISTS SONATA_LUAU_TARGETS)
    set(fixed "")
    get_target_property(dirs ${t} INTERFACE_INCLUDE_DIRECTORIES)
    if(dirs)
      foreach(d IN LISTS dirs)
        list(APPEND fixed "$<BUILD_INTERFACE:${d}>")
      endforeach()
    endif()
    list(APPEND fixed "$<INSTALL_INTERFACE:${SONATA_LUAU_INCLUDE_DEST}>")
    set_target_properties(${t} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${fixed}")
  endforeach()

  # Install Luau's public headers somewhere private to sonata
  foreach(sub Common Ast Compiler VM Bytecode)
    install(DIRECTORY ${PROJECT_SOURCE_DIR}/external/luau/${sub}/include/
            DESTINATION ${SONATA_LUAU_INCLUDE_DEST})
  endforeach()

  install(TARGETS sonata ${SONATA_LUAU_TARGETS} EXPORT sonataTargets)
  install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
          FILES_MATCHING PATTERN "*.hpp")
  install(EXPORT sonataTargets NAMESPACE sonata:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sonata)

  configure_package_config_file(${PROJECT_SOURCE_DIR}/cmake/sonataConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/sonataConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sonata)
  write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/sonataConfigVersion.cmake
    COMPATIBILITY SameMinorVersion)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/sonataConfig.cmake
                ${CMAKE_CURRENT_BINARY_DIR}/sonataConfigVersion.cmake
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sonata)
endif()
