# CMakeLists.txt
cmake_minimum_required(VERSION 3.21)
project(sonata VERSION 0.1.8 LANGUAGES CXX)

if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_CXX_EXTENSIONS OFF)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

option(SONATA_BUILD_CLI   "Build the sn executable" ${PROJECT_IS_TOP_LEVEL})
option(SONATA_INSTALL     "Generate install rules"  ${PROJECT_IS_TOP_LEVEL})
option(SONATA_BUILD_TESTS "Build unit tests"        ${PROJECT_IS_TOP_LEVEL})

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
endif()

add_subdirectory(src)
if(SONATA_BUILD_CLI)
  add_subdirectory(cli)
endif()

if(SONATA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(SONATA_INSTALL)
  set(CPACK_PACKAGE_NAME "sonata")
  set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
  set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
  
  if(WIN32)
    set(CPACK_GENERATOR "ZIP")
  else()
    set(CPACK_GENERATOR "TGZ")
  endif()
  
  include(CPack)
endif()