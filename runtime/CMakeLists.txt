# runtime/CMakeLists.txt
#
# snrt: the executable "sn build" turns into standalone programs (it appends
# the compiled program to a copy of it). Only the built-in libraries in
# SONATA_LIBS are compiled in, and it never contains the CLI or the package
# manager, so it's linked to be as small and as self-contained as possible.
include(GNUInstallDirs)

add_executable(snrt main.cpp)
target_link_libraries(snrt PRIVATE sonata::runtime)

# Always <build>/runtime/snrt, even with multi-config generators, so that
# "sn build" (and a tailored build of it) knows where to look.
set_target_properties(snrt PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/runtime>")

if(MSVC)
  target_link_options(snrt PRIVATE "$<$<CONFIG:Release,MinSizeRel>:/OPT:REF;/OPT:ICF>")
else()
  # Drop unreferenced code (see -ffunction-sections in the root CMakeLists)
  # and, for release builds, the symbol table.
  if(APPLE)
    target_link_options(snrt PRIVATE -Wl,-dead_strip $<$<CONFIG:Release,MinSizeRel>:-Wl,-S,-x>)
  else()
    target_link_options(snrt PRIVATE -Wl,--gc-sections $<$<CONFIG:Release,MinSizeRel>:-s>)
  endif()

  # libstdc++ isn't guaranteed to be on the machine a program ends up on.
  # Android (e.g. Termux) has no static libc++, so the flag makes clang look
  # for a libc++_shared.a that doesn't exist.
  if(SONATA_STATIC_DEPS AND NOT APPLE AND NOT ANDROID
     AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(snrt PRIVATE -static-libstdc++ -static-libgcc)
  endif()
endif()

if(SONATA_STANDALONE AND NOT MSVC)
  target_link_options(snrt PRIVATE -static)

  include(CheckCXXSourceCompiles)
  check_cxx_source_compiles("
    #include <cstdio>
    #ifndef __GLIBC__
    #error not glibc
    #endif
    int main() { return 0; }" SONATA_USES_GLIBC)

  if(SONATA_USES_GLIBC AND "net" IN_LIST SONATA_LIBS)
    message(WARNING
      "SONATA_STANDALONE with glibc: the program links fully statically, but "
      "glibc still loads its NSS modules at runtime for DNS lookups in "
      "@sonata/net, which only works with a matching glibc. For a program "
      "that runs anywhere, build with a musl toolchain (Alpine, or musl-gcc).")
  endif()
endif()

if(SONATA_INSTALL)
  install(TARGETS snrt RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/sonata)
endif()
