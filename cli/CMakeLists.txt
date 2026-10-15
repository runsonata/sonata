# cli/CMakeLists.txt
include(GNUInstallDirs)

# ---- Luau Language Server types ----
# "sn init --lsp" writes the files in types/ into a project's .sonata/types,
# so they are compiled into sn as byte arrays. Editing one reconfigures.
file(GLOB_RECURSE sonata_type_files CONFIGURE_DEPENDS
     RELATIVE ${PROJECT_SOURCE_DIR}/types ${PROJECT_SOURCE_DIR}/types/*.luau)
list(SORT sonata_type_files)

set(SONATA_TYPE_ARRAYS "")
set(SONATA_TYPE_ENTRIES "")
set(sonata_type_index 0)
foreach(file IN LISTS sonata_type_files)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/types/${file})
  file(READ ${PROJECT_SOURCE_DIR}/types/${file} bytes HEX)
  string(REGEX REPLACE "(..)" "0x\\1," bytes "${bytes}")
  string(APPEND SONATA_TYPE_ARRAYS "const unsigned char file${sonata_type_index}[] = {${bytes}};\n")
  string(APPEND SONATA_TYPE_ENTRIES "    {\"${file}\", file${sonata_type_index}, sizeof(file${sonata_type_index})},\n")
  math(EXPR sonata_type_index "${sonata_type_index} + 1")
endforeach()

configure_file(${PROJECT_SOURCE_DIR}/cmake/lsp_types.cpp.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/lsp_types.cpp @ONLY)

add_library(sonata_cli STATIC cli.cpp lsp.cpp commands/init.cpp commands/pkg.cpp commands/run.cpp commands/build.cpp
            ${CMAKE_CURRENT_BINARY_DIR}/generated/lsp_types.cpp)
target_include_directories(sonata_cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sonata_cli PUBLIC sonata::sonata)
target_compile_definitions(sonata_cli PRIVATE BUILD_TYPE="$<CONFIG>")

# "sn build" builds tailored runtimes from this source tree while it still
# exists (a developer's build); installed copies fetch the matching release.
target_compile_definitions(sonata_cli PRIVATE SONATA_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

add_executable(sn main.cpp)
target_link_libraries(sn PRIVATE sonata_cli)

install(TARGETS sn RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# A dynamically linked OpenSSL has to ship next to sn.exe.
if(WIN32 AND "net" IN_LIST SONATA_LIBS AND NOT SONATA_OPENSSL_STATIC)
  get_filename_component(_sonata_ssl_root "${OPENSSL_INCLUDE_DIR}" DIRECTORY)
  file(GLOB _sonata_ssl_dlls
    "${_sonata_ssl_root}/bin/libssl-*.dll"
    "${_sonata_ssl_root}/bin/libcrypto-*.dll")
  if(_sonata_ssl_dlls)
    install(FILES ${_sonata_ssl_dlls} DESTINATION ${CMAKE_INSTALL_BINDIR})
  else()
    message(WARNING "OpenSSL DLLs not found in ${_sonata_ssl_root}/bin; "
                    "the packaged sn.exe will need OpenSSL on PATH")
  endif()
endif()