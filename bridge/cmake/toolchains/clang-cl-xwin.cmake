# clang-cl-xwin.cmake — cross-build the Windows platform layer from Linux with
# the MSVC ABI (ADR 0001, "Development without a Windows or macOS machine").
#
#   cmake -S bridge -B build-win -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=bridge/cmake/toolchains/clang-cl-xwin.cmake \
#         -DCK3_PLATFORM=windows -DBUILD_BRIDGE_TESTS=ON
#   cmake --build build-win && ctest --test-dir build-win     # tests run under wine
#
# Inputs (cache variables or environment variables of the same name):
#   CK3_XWIN_DIR      the `xwin splat --output` directory (crt/ and sdk/ inside)
#                     default: $HOME/.local/opt/xwin-sdk
#   CK3_LLVM_BIN      a directory holding clang-cl, lld-link and llvm-lib
#                     (symlinks to clang/lld are enough); default: $HOME/.local/opt/clangcl-bin
#   CK3_WINE          the test emulator; default: wine on PATH. Set it empty to
#                     build without registering an emulator.
#
# How this host got them (2026-10-18, documented in docs/porting/README.md):
#   llvm-mingw 20261006 (clang 23.1.3) unpacked under ~/.local/opt, with
#   clang-cl/lld-link/llvm-lib symlinks in ~/.local/opt/clangcl-bin; xwin 0.10.0
#   `xwin --accept-license splat --output ~/.local/opt/xwin-sdk` (the Microsoft
#   CRT and Windows SDK; the licence is the downloader's to accept).
# The windows-latest CI leg does not use this file: it builds with real MSVC.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

foreach(_v CK3_XWIN_DIR CK3_LLVM_BIN CK3_WINE)
    if(NOT DEFINED ${_v} AND DEFINED ENV{${_v}})
        set(${_v} "$ENV{${_v}}")
    endif()
endforeach()
if(NOT DEFINED CK3_XWIN_DIR)
    set(CK3_XWIN_DIR "$ENV{HOME}/.local/opt/xwin-sdk")
endif()
if(NOT DEFINED CK3_LLVM_BIN)
    set(CK3_LLVM_BIN "$ENV{HOME}/.local/opt/clangcl-bin")
endif()
set(CK3_XWIN_DIR "${CK3_XWIN_DIR}" CACHE PATH "xwin splat output directory")
set(CK3_LLVM_BIN "${CK3_LLVM_BIN}" CACHE PATH "directory with clang-cl, lld-link, llvm-lib")
if(NOT EXISTS "${CK3_XWIN_DIR}/crt/include" OR NOT EXISTS "${CK3_XWIN_DIR}/sdk/include/um")
    message(FATAL_ERROR "CK3_XWIN_DIR='${CK3_XWIN_DIR}' has no crt/include or sdk/include/um; "
        "run `xwin --accept-license splat --output <dir>` first")
endif()

set(CMAKE_C_COMPILER   "${CK3_LLVM_BIN}/clang-cl")
set(CMAKE_CXX_COMPILER "${CK3_LLVM_BIN}/clang-cl")
set(CMAKE_LINKER       "${CK3_LLVM_BIN}/lld-link")
set(CMAKE_AR           "${CK3_LLVM_BIN}/llvm-lib")
set(CMAKE_RC_COMPILER  "${CK3_LLVM_BIN}/llvm-rc")
# llvm-mingw ships no llvm-mt; without one, link with /MANIFEST:NO (nothing here
# needs an embedded manifest) instead of failing in CMake's manifest step.
if(EXISTS "${CK3_LLVM_BIN}/llvm-mt")
    set(CMAKE_MT "${CK3_LLVM_BIN}/llvm-mt")
    set(_nomanifest "")
else()
    set(_nomanifest " /MANIFEST:NO")
endif()

set(_xw "${CK3_XWIN_DIR}")
set(_inc "-imsvc ${_xw}/crt/include -imsvc ${_xw}/sdk/include/ucrt -imsvc ${_xw}/sdk/include/um -imsvc ${_xw}/sdk/include/shared")
set(CMAKE_C_FLAGS_INIT   "${_inc}")
set(CMAKE_CXX_FLAGS_INIT "${_inc} /EHsc")
set(_lib "/libpath:${_xw}/crt/lib/x86_64 /libpath:${_xw}/sdk/lib/um/x86_64 /libpath:${_xw}/sdk/lib/ucrt/x86_64")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "${_lib}${_nomanifest}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_lib}${_nomanifest}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_lib}${_nomanifest}")
# xwin ships the release CRT only (no msvcrtd.lib), and a static CRT needs no
# VC++ redistributable under wine: /MT everywhere, probes built as Release.
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CK3_XWIN_DIR CK3_LLVM_BIN)

if(NOT DEFINED CK3_WINE)
    find_program(CK3_WINE wine)
endif()
if(CK3_WINE)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${CK3_WINE}")
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
