#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

# Extra arguments for every configure.
cmake_args=()

# Windows: use vcpkg's libraries when theyre installed for
# $VCPKG_DEFAULT_TRIPLET (default x64-windows-static-md static libraries
# built for the dynamic CRT, so nothing needs DLLs next to it). VCPKG_ROOT is
# vcpkg's own variable; GitHub's Windows runners set VCPKG_INSTALLATION_ROOT.
# A CMAKE_TOOLCHAIN_FILE already set in the environment always wins.
using_vcpkg=0
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*)
    vcpkg_root="${VCPKG_ROOT:-${VCPKG_INSTALLATION_ROOT:-}}"
    vcpkg_triplet="${VCPKG_DEFAULT_TRIPLET:-x64-windows-static-md}"
    if [[ -z "${CMAKE_TOOLCHAIN_FILE:-}" && -n "$vcpkg_root" ]] &&
       [[ -d "$(cygpath -u "$vcpkg_root")/installed/$vcpkg_triplet" ]]; then
      vcpkg_root="$(cygpath -m "$vcpkg_root")"
      cmake_args+=("-DCMAKE_TOOLCHAIN_FILE=$vcpkg_root/scripts/buildsystems/vcpkg.cmake"
                   "-DVCPKG_TARGET_TRIPLET=$vcpkg_triplet")
      using_vcpkg=1
    fi
    ;;
esac

# Help CMake's FindOpenSSL on platforms where OpenSSL isnt in a default
# location (Homebrew keg on macOS, Program Files on Windows without vcpkg).
# An OPENSSL_ROOT_DIR already set in the environment always wins.
if [[ -z "${OPENSSL_ROOT_DIR:-}" && "$using_vcpkg" == 0 ]]; then
  case "$(uname -s)" in
    Darwin)
      if command -v brew >/dev/null 2>&1; then
        ssl_prefix="$(brew --prefix openssl@3 2>/dev/null || true)"
        if [[ -n "$ssl_prefix" && -d "$ssl_prefix/include/openssl" ]]; then
          export OPENSSL_ROOT_DIR="$ssl_prefix"
        fi
      fi
      ;;
    MINGW*|MSYS*|CYGWIN*)
      for ssl_prefix in "/c/Program Files/OpenSSL" "/c/Program Files/OpenSSL-Win64"; do
        if [[ -f "$ssl_prefix/include/openssl/ssl.h" ]]; then
          export OPENSSL_ROOT_DIR="$(cygpath -m "$ssl_prefix")"
          break
        fi
      done
      ;;
  esac
fi

cmd="${1:-debug}"

# "${cmake_args[@]}" fails under set -u when empty in bash 3.2 (macOS).
configure() { cmake --preset "$1" ${cmake_args[@]+"${cmake_args[@]}"}; }

case "$cmd" in
  debug|release)
    configure "$cmd"
    cmake --build --preset "$cmd" --parallel
    ln -sf "build/$cmd/compile_commands.json" compile_commands.json   # for clangd
    ;;
  package)  configure package
            cmake --build --preset package --parallel
            cpack --config build/package/CPackConfig.cmake -C Release -B build/package
            ;;
  run)      shift; ./build/debug/cli/sn "$@" ;;
  install)  cmake --install build/release --prefix "${PREFIX:-$HOME/.local}" ;;
  clean)    rm -rf build compile_commands.json ;;
  analyze)  command -v clang-tidy >/dev/null || { echo "analyze: clang-tidy not installed" >&2; exit 1; }
            [[ -f compile_commands.json ]]    || { echo "analyze: no compile_commands.json, run ./b.sh debug first" >&2; exit 1; }
            JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu)"
            EXCLUDE_PATTERN="^$PWD/(external|build)/"
            HEADER_FILTER="^$PWD/(src|include|cli)/"
            FILES=$(jq -r '.[].file' compile_commands.json | { grep -vE "$EXCLUDE_PATTERN" || true; } | sort -u)
            [[ -n "$FILES" ]] || { echo "analyze: no source files found" >&2; exit 1; }
            LINE_COUNT=$(echo "$FILES" | xargs cat | wc -l | tr -d ' ')
            OUTPUT=$(echo "$FILES" | tr '\n' '\0' \
                     | xargs -0 -n1 -P "$JOBS" clang-tidy -p . --quiet --header-filter="$HEADER_FILTER" 2>&1 || true)
            DIAGS=$(grep -E "warning:|error:|Error while processing" <<<"$OUTPUT" | sort -u || true)
            WARNINGS=$(grep -c "warning:" <<<"$DIAGS" || true)
            ERRORS=$(grep -cE "error:|Error while processing" <<<"$DIAGS" || true)

            [[ -z "$DIAGS" ]] || echo "$DIAGS"
            echo "${LINE_COUNT} lines, ${WARNINGS} warnings and ${ERRORS} errors"

            (( ERRORS == 0 )) || exit 1
            # (( WARNINGS == 0 )) || exit 1 # uncomment for a strict gate
            ;;
  test)
            shift
            preset="debug"
            if [[ "${1:-}" == "debug" || "${1:-}" == "release" ]]; then
              preset="$1"; shift
            fi
            configure "$preset"
            cmake --build --preset "$preset" --parallel
            # -C picks the build to test with multi-config generators (Visual
            # Studio, the default on Windows); single-config ones ignore it.
            config="Debug"; [[ "$preset" == "release" ]] && config="Release"
            ctest --test-dir "build/$preset" -C "$config" --output-on-failure \
                  --parallel "$(nproc 2>/dev/null || sysctl -n hw.ncpu)" "$@"
            ;;
  *)        echo "usage: $0 [debug|release|package|run|install|clean|analyze|test]"; exit 1 ;;
esac