# tests/CMakeLists.txt
set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${PROJECT_SOURCE_DIR}/external/doctest
                 ${CMAKE_BINARY_DIR}/external/doctest EXCLUDE_FROM_ALL)
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/external/doctest/scripts/cmake)

add_executable(sonata_tests
  main.cpp
  test_luau.cpp
  test_builtins.cpp
  test_build.cpp
  test_runtime.cpp
  test_lsp.cpp
)

target_link_libraries(sonata_tests PRIVATE
  sonata
  doctest::doctest
)

# test_lsp.cpp tests the CLI's Luau Language Server support.
if(TARGET sonata_cli)
  target_link_libraries(sonata_tests PRIVATE sonata_cli)
else()
  set_source_files_properties(test_lsp.cpp PROPERTIES HEADER_FILE_ONLY ON)
endif()

# Only needed if tests include private headers such as src/core/libs/libs.hpp
target_include_directories(sonata_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)

include(doctest)
doctest_discover_tests(sonata_tests)