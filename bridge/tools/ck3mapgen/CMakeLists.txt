# ck3mapgen — standalone build-time CK3 map-processor (P1).
#
# This is intentionally self-contained: it links NOTHING from the bridge
# runtime and depends only on the vendored stb_image.h + the C++17 stdlib.
# It can be built on its own:
#
#   cmake -S bridge/tools/ck3mapgen -B build-mapgen
#   cmake --build build-mapgen
#
# or pulled into the bridge build via the optional add_subdirectory guard in
# bridge/CMakeLists.txt (-DCK3_BUILD_MAPGEN=ON). It does NOT run during make;
# generating the artifact is a separate `ck3mapgen --game-dir ... --out ...`
# invocation (wired into scripts/bdt.sh mapgen).

cmake_minimum_required(VERSION 3.16)
project(ck3mapgen LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(ck3mapgen ck3mapgen.cpp)

# stb_image.h lives in bridge/third_party (two levels up from this dir).
target_include_directories(ck3mapgen PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party
)

target_compile_options(ck3mapgen PRIVATE -O2 -Wall -Wextra)
