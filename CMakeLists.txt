# CMakeLists.txt
cmake_minimum_required(VERSION 3.21)
project(sonata VERSION 1.0.1 LANGUAGES CXX)

if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_CXX_EXTENSIONS OFF)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

option(SONATA_BUILD_CLI     "Build the sn executable"                ${PROJECT_IS_TOP_LEVEL})
option(SONATA_BUILD_RUNTIME "Build snrt, the runtime 'sn build' uses" ${PROJECT_IS_TOP_LEVEL})
option(SONATA_INSTALL       "Generate install rules"                 ${PROJECT_IS_TOP_LEVEL})
option(SONATA_BUILD_TESTS   "Build unit tests"                       ${PROJECT_IS_TOP_LEVEL})

# How the runtime links (see runtime/CMakeLists.txt):
#   SONATA_STATIC_DEPS  OpenSSL, zlib and the C++ runtime are linked in
#                       statically where static copies exist, so a built
#                       program only needs the OS's own C library.
#   SONATA_STANDALONE   No shared libraries at all (fully static on Linux,
#                       static CRT on Windows). Not possible on macOS.
option(SONATA_STATIC_DEPS "Link third-party dependencies statically when possible" ON)
option(SONATA_STANDALONE  "Build a runtime with no shared-library dependencies"    OFF)

if(SONATA_STANDALONE)
  if(APPLE)
    message(FATAL_ERROR "SONATA_STANDALONE: macOS has no static system libraries, "
                        "so a fully static program can't be built there.")
  endif()
  set(SONATA_STATIC_DEPS ON CACHE BOOL "" FORCE)
  if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set(LUAU_STATIC_CRT ON CACHE BOOL "" FORCE)
  endif()
endif()

# Lets the linker drop every function the runtime never calls (unused Luau
# compiler code, unused parts of each library, ...). Harmless for sn.
if(NOT MSVC)
  add_compile_options(-ffunction-sections -fdata-sections)
endif()

# Optional compress backends (zlib is always required).
# The default comes from an environment variable of the same name, so CI can
# enable them without changing b.sh. An explicit -D<NAME>=... always wins.
function(sonata_env_option name doc)
  set(default OFF)
  if(DEFINED ENV{${name}} AND "$ENV{${name}}")
    set(default ON)
  endif()
  option(${name} "${doc}" ${default})
endfunction()

sonata_env_option(SONATA_COMPRESS_ZSTD   "Enable zstd in the compress library (needs libzstd)")
sonata_env_option(SONATA_COMPRESS_CRYPTO "Enable password support in the compress library (needs libsodium)")

set(SONATA_BUILD_TYPE "")

set(SONATA_OWNS_LUAU OFF)
if(NOT TARGET Luau.VM)
  set(LUAU_BUILD_CLI   OFF CACHE BOOL "" FORCE)
  set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  add_subdirectory(external/luau)
  set(SONATA_OWNS_LUAU ON)
endif()

find_package(Git QUIET)
set(SONATA_GIT_HASH "")
if(Git_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    OUTPUT_VARIABLE SONATA_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)

  execute_process(
    COMMAND ${GIT_EXECUTABLE} describe --tags --always
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/external/luau
    OUTPUT_VARIABLE SONATA_LUAU_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
endif()

add_subdirectory(src)
if(SONATA_BUILD_RUNTIME)
  add_subdirectory(runtime)
endif()
if(SONATA_BUILD_CLI)
  add_subdirectory(cli)
endif()

if(SONATA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(SONATA_INSTALL)
  # Packages are named sonata-<version>-<os>-<arch>, with the same os/arch
  # names as info::platform_string(): linux|macos|windows and x64|arm64.
  if(WIN32)
    set(sonata_package_os windows)
  elseif(APPLE)
    set(sonata_package_os macos)
  else()
    string(TOLOWER "${CMAKE_SYSTEM_NAME}" sonata_package_os)
  endif()

  # The architecture being built for: MSVC knows it even when it isn't the
  # host's (CMAKE_SYSTEM_PROCESSOR is the host's there), and on macOS
  # CMAKE_OSX_ARCHITECTURES picks it.
  if(MSVC AND CMAKE_CXX_COMPILER_ARCHITECTURE_ID)
    set(sonata_package_arch "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")
  elseif(APPLE AND CMAKE_OSX_ARCHITECTURES)
    set(sonata_package_arch "${CMAKE_OSX_ARCHITECTURES}")
  else()
    set(sonata_package_arch "${CMAKE_SYSTEM_PROCESSOR}")
  endif()
  string(TOLOWER "${sonata_package_arch}" sonata_package_arch)
  if(sonata_package_arch MATCHES ";")
    set(sonata_package_arch universal)
  elseif(sonata_package_arch MATCHES "^(x86_64|amd64|x64)$")
    set(sonata_package_arch x64)
  elseif(sonata_package_arch MATCHES "^(aarch64|arm64)$")
    set(sonata_package_arch arm64)
  endif()

  set(CPACK_PACKAGE_NAME "sonata")
  set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
  if(SONATA_VERSION_PRE)
    string(APPEND CPACK_PACKAGE_VERSION "-${SONATA_VERSION_PRE}")
  endif()
  set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${sonata_package_os}-${sonata_package_arch}")

  if(WIN32)
    set(CPACK_GENERATOR "ZIP")
  else()
    set(CPACK_GENERATOR "TGZ")
  endif()
  
  include(CPack)
endif()