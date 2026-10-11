# cli/CMakeLists.txt
include(GNUInstallDirs)

add_library(sonata_cli STATIC cli.cpp commands/init.cpp commands/pkg.cpp commands/run.cpp commands/build.cpp)
target_include_directories(sonata_cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sonata_cli PUBLIC sonata::sonata)
target_compile_definitions(sonata_cli PRIVATE BUILD_TYPE="${CMAKE_BUILD_TYPE}")

# "sn build" builds tailored runtimes from this source tree while it still
# exists (a developer's build); installed copies fetch the matching release.
target_compile_definitions(sonata_cli PRIVATE SONATA_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

add_executable(sn main.cpp)
target_link_libraries(sn PRIVATE sonata_cli)

install(TARGETS sn RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})