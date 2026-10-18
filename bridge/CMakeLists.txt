cmake_minimum_required(VERSION 3.16)

# Release version: ../VERSION in a public tree (written by release/export.py,
# CalVer "2026.10.0"); the research checkout has none and keeps 0.1.0.
# CK3_BRIDGE_VERSION is the full string; project() gets its numeric prefix so
# a suffixed version ("2026.10.0rc2") still configures.
set(CK3_BRIDGE_VERSION "0.1.0")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" _ck3_ver)
    string(STRIP "${_ck3_ver}" CK3_BRIDGE_VERSION)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION")
endif()
if(NOT CK3_BRIDGE_VERSION MATCHES "^([0-9]+(\\.[0-9]+)*)")
    message(FATAL_ERROR "VERSION '${CK3_BRIDGE_VERSION}' does not start with a number")
endif()
project(ck3_bridge VERSION ${CMAKE_MATCH_1} LANGUAGES CXX C)
# Global, so every target that compiles the handshake code sees it.
add_compile_definitions(CK3_BRIDGE_VERSION="${CK3_BRIDGE_VERSION}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ═════════════════════════════════════════════════════════════════════════════
# PLATFORM LAYER — BEGIN (docs/adr/0001-windows-macos-port.md, phase P2)
#
# CK3_PLATFORM picks the one bridge/src/os/<platform>/ implementation that is
# compiled (interfaces: include/os/os.h).
#
#   linux    the default and the only platform that builds the bridge. The
#            layer is a separate static library, ck3_os, that ck3_bridge does
#            NOT link yet: libck3_bridge.so is built exactly as before, from
#            the same sources with the same flags and the same four exports.
#            Routing the bridge through bridge::os is the rest of P2.
#   windows  build ONLY ck3_os (typed-refusal stubs) and its test, then stop:
#   macos    the rest of this file is the Linux bridge and is never reached.
#            The windows-latest and macos-14 CI legs are the proof the stubs
#            compile with MSVC and Apple clang.
#
# Tests (BUILD_BRIDGE_TESTS, declared with the other tests further down; read
# here from the cache): test_os_linux_adapters on linux, plus both stub TUs
# compiled as strict C++17 by the host compiler (test_os_stubs_windows,
# test_os_stubs_macos); test_os_stubs on windows/macos.
# ═════════════════════════════════════════════════════════════════════════════
set(CK3_PLATFORM "linux" CACHE STRING "Platform layer implementation: linux|windows|macos")
set_property(CACHE CK3_PLATFORM PROPERTY STRINGS linux windows macos)
if(NOT CK3_PLATFORM MATCHES "^(linux|windows|macos)$")
    message(FATAL_ERROR "CK3_PLATFORM='${CK3_PLATFORM}': must be linux, windows or macos")
endif()
if(CK3_PLATFORM STREQUAL "linux" AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "CK3_PLATFORM=linux on a ${CMAKE_SYSTEM_NAME} host: the bridge "
        "is Linux-only. Configure with -DCK3_PLATFORM=windows or -DCK3_PLATFORM=macos "
        "to build the platform-layer stubs.")
endif()
message(STATUS "ck3 platform layer: ${CK3_PLATFORM}")

if(MSVC)
    set(CK3_OS_WARNINGS /W4 /permissive-)
else()
    set(CK3_OS_WARNINGS -Wall -Wextra)
endif()

if(CK3_PLATFORM STREQUAL "linux")
    add_library(ck3_os STATIC src/os/os_common.cpp src/os/linux/os_linux.cpp)
    # The adapters forward to bridge code (bridge.h, instance_paths.cpp,
    # build_id_witness.cpp); a consumer links those sources itself, so ck3_os
    # never carries a second copy of them.
    target_compile_definitions(ck3_os PRIVATE _GNU_SOURCE)
else()
    add_library(ck3_os STATIC src/os/os_common.cpp src/os/${CK3_PLATFORM}/stubs.cpp)
    set_target_properties(ck3_os PROPERTIES CXX_EXTENSIONS OFF)
endif()
target_include_directories(ck3_os PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(ck3_os PRIVATE ${CK3_OS_WARNINGS})

if(BUILD_BRIDGE_TESTS)
    enable_testing()
    if(CK3_PLATFORM STREQUAL "linux")
        find_package(Threads REQUIRED)
        add_executable(test_os_linux_adapters
            src/os/test/test_os_linux_adapters.cpp
            src/instance_paths.cpp
            src/build_id_witness.cpp
        )
        target_link_libraries(test_os_linux_adapters PRIVATE ck3_os Threads::Threads ${CMAKE_DL_LIBS})
        target_compile_definitions(test_os_linux_adapters PRIVATE
            CK3_BRIDGE_VERSION_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/bridge.version")
        target_compile_options(test_os_linux_adapters PRIVATE -O2 ${CK3_OS_WARNINGS})
        # ck3 is PIE; the image-base checks need a PIE main executable too.
        target_link_options(test_os_linux_adapters PRIVATE -pie)
        add_test(NAME test_os_linux_adapters COMMAND test_os_linux_adapters)

        foreach(_p windows macos)
            add_executable(test_os_stubs_${_p}
                src/os/test/test_os_stubs.cpp
                src/os/os_common.cpp
                src/os/${_p}/stubs.cpp
            )
            target_include_directories(test_os_stubs_${_p} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
            target_compile_definitions(test_os_stubs_${_p} PRIVATE CK3_OS_EXPECT_PLATFORM="${_p}")
            set_target_properties(test_os_stubs_${_p} PROPERTIES CXX_EXTENSIONS OFF)
            target_compile_options(test_os_stubs_${_p} PRIVATE -pedantic-errors -Werror ${CK3_OS_WARNINGS})
            add_test(NAME test_os_stubs_${_p} COMMAND test_os_stubs_${_p})
        endforeach()
    else()
        add_executable(test_os_stubs src/os/test/test_os_stubs.cpp)
        target_link_libraries(test_os_stubs PRIVATE ck3_os)
        target_compile_definitions(test_os_stubs PRIVATE CK3_OS_EXPECT_PLATFORM="${CK3_PLATFORM}")
        set_target_properties(test_os_stubs PROPERTIES CXX_EXTENSIONS OFF)
        target_compile_options(test_os_stubs PRIVATE ${CK3_OS_WARNINGS})
        add_test(NAME test_os_stubs COMMAND test_os_stubs)
    endif()
endif()

if(NOT CK3_PLATFORM STREQUAL "linux")
    return()   # windows/macos: the platform layer is all there is to build
endif()
# ═════════════════════════════════════════════════════════════════════════════
# PLATFORM LAYER — END
# ═════════════════════════════════════════════════════════════════════════════

# Build as shared library (.so) for LD_PRELOAD injection
add_library(ck3_bridge SHARED
    src/bridge_init.cpp
    src/hooks.cpp
    src/ipc_server.cpp
    src/client_slot.cpp
    src/game_state.cpp
    src/observation.cpp
    src/dispatch_router.cpp
    src/json_helpers.cpp
    src/query_handlers.cpp
    src/action_handlers.cpp
    src/personality.cpp
    src/personality_world_reset.cpp
    # ck-9fc3 — the save/world IDENTITY and the cross-process lock carrier.
    # Both ENGINE-FREE, so bridge/test/acceptance_save_identity links them
    # verbatim and proves the negative control with no CK3 running.
    src/personality_persist.cpp
    src/world_identity.cpp
    src/ai_eval_hook.cpp
    src/event_handler.cpp
    src/name_tables.cpp
    src/world_epoch.cpp
    src/ai_subsystem_hooks.cpp
    src/runtime_resolver.cpp
    src/build_id_witness.cpp      # ck-nkyc leg (e): the observed BuildID + strictness witness
    src/sig_scanner.cpp
    src/intent_dispatch.cpp
    src/snapshot.cpp
    src/session.cpp
    src/deferred_drain.cpp
    src/war_score_hook.cpp
    src/war_declaration_hook.cpp
    # ck-sd4d — the ended-war ledger: a PURE half (war_history.cpp, compiled
    # standalone by test_war_history) and the WAR_END_CORE hook + IPC handler.
    src/war_history.cpp
    src/war_history_hook.cpp
    src/scheme_init_hook.cpp
    src/ai_cb_selector_hook.cpp
    src/marriage_observation_hook.cpp
    src/council_task_hook.cpp
    src/council_suppress_hook.cpp
    src/offgoal_war_veto_hook.cpp
    src/offset_validator.cpp
    src/phase3_probes.cpp
    src/named_trigger_eval.cpp
    src/script_block_parser.cpp
    src/launcher_bypass.cpp
    src/lobby_view_hook.cpp
    src/map_graph.cpp
    # ck-6kvq.8/.9 — the event pipeline: a PURE scheduler + a PURE
    # ticket/apply layer + the engine-facing conductor. The first three
    # are also compiled standalone by test_pipeline (no CK3 binary).
    src/flush_scheduler.cpp
    src/ticket_table.cpp
    src/flush_apply.cpp
    src/pipeline.cpp
    # ck-6kvq.4 / ck-1ek9 — the mode policy table. PURE; compiled standalone by
    # test_pipeline_mode. NOTE FOR HOST TARGETS: dispatch_router.cpp and
    # ipc_server.cpp both call into this TU, so any test target that compiles
    # either of them must list src/pipeline_mode.cpp too or it will not link
    # (this is how ck-1ek9 broke test_release_handler, which was added after it
    # cut its branch — a CLEAN git merge, a semantic collision only the build
    # could see).
    src/pipeline_mode.cpp
    # ck-p2sb — the deferred coordinator's refusal counts. A LEAF TU for the same
    # reason pipeline_mode.cpp is one: dispatch_router.cpp reports them, so every
    # host target that compiles dispatch_router.cpp needs this too. They are NOT
    # in deferred_drain.cpp because that TU pulls in is_paused()/game_get_speed().
    src/clock_leak_backstop.cpp
    src/deferred_counters.cpp
    # ck-6kvq.13 — R6: the decision-budget ledger. PURE; compiled standalone
    # by test_budget_ledger.
    src/budget_ledger.cpp
    src/action_args.cpp
    # ck-6kvq.19 — the conductor's payload store + drain correlation, keyed by
    # the BRIDGE-MINTED ticket. PURE (rapidjson only); compiled standalone by
    # test_drain_registry.
    src/drain_registry.cpp
    # ck-6kvq.10 — R1c: the pulse ledger; R3-as-reversed: the episode policy
    # and the unavailability ledger. All PURE (no engine deps) and compiled
    # standalone by their own tests.
    src/pulse_ledger.cpp
    src/episode_policy.cpp
    src/unavailability.cpp
    # ck-ijfa: the MEASURED game-days-per-wall-second of the running condition —
    # the only honest conversion between the covariate's unit and the unit every
    # question about it is asked in. PURE, host-tested by test_clock_rate.
    src/clock_rate.cpp
    # ck-6kvq.12 — R4: the shared append-only board. PURE (no engine deps; the
    # calendar never appears in it) and compiled standalone by test_board and
    # acceptance_board.
    src/board.cpp
    src/board_persist.cpp
    # ck-qn03 (R12): the instance naming rules — which socket to bind, which
    # telemetry dir to append to, which log to open. PURE (libc only) and
    # compiled standalone by test_instance_paths.
    src/instance_paths.cpp
    src/log_archive.cpp
)

# Build provenance — regenerated every build (git SHA, dirty state, offsets MD5,
# build time) so a deployed .so can always be matched to its source commit.
add_custom_target(gen_provenance ALL
    COMMAND ${CMAKE_COMMAND}
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/build_provenance.h
        -DSRCDIR=${CMAKE_CURRENT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_provenance.cmake
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/build_provenance.h
    COMMENT "Generating build provenance header"
)
add_dependencies(ck3_bridge gen_provenance)
target_include_directories(ck3_bridge PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

target_include_directories(ck3_bridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

# We need pthreads for the IPC server thread
find_package(Threads REQUIRED)
target_link_libraries(ck3_bridge PRIVATE Threads::Threads)

# Link dl for dlopen/dlsym (ASLR base resolution)
target_link_libraries(ck3_bridge PRIVATE ${CMAKE_DL_LIBS})

# Zydis 5.0.0 + Zycore 1.5.2. Exactly one header set per build:
#   vendored (default when lib64/ exists): third_party/zydis/lib64 + include/.
#   source (public tree, or -DCK3_ZYDIS_FROM_SOURCE=ON): pinned archives;
#     the vendored include/ is never on the path.
# The pin is the Zydis commit whose include/ matches the vendored headers byte
# for byte (a95bb71, the rc1 pin, differs in 7 files). Configure fails if the
# two sets drift apart.
set(ZYDIS_PREFIX ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zydis)
option(CK3_ZYDIS_FROM_SOURCE "Build Zydis from the pinned archive even if lib64/ exists" OFF)
if(EXISTS "${ZYDIS_PREFIX}/lib64/libZydis.a" AND NOT CK3_ZYDIS_FROM_SOURCE)
    target_include_directories(ck3_bridge PRIVATE ${ZYDIS_PREFIX}/include)
    target_link_libraries(ck3_bridge PRIVATE
        ${ZYDIS_PREFIX}/lib64/libZydis.a
        ${ZYDIS_PREFIX}/lib64/libZycore.a
    )
else()
    include(FetchContent)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)   # extracted files get extraction mtime
    endif()
    # Zycore is a submodule of Zydis, so the Zydis archive lacks it. Fetched
    # at the submodule commit; SOURCE_SUBDIR has no CMakeLists.txt, so this
    # only downloads. Zydis adds it through ZYAN_ZYCORE_PATH.
    FetchContent_Declare(zycore
        URL      https://github.com/zyantific/zycore-c/archive/75a36c45ae1ad382b0f4e0ede0af84c11ee69928.tar.gz
        URL_HASH SHA256=c24ebd402d0ce83cd54f08e68189e68d54e3b5938f65ca972dfdbac89757139b
        SOURCE_SUBDIR _populate_only
    )
    FetchContent_MakeAvailable(zycore)
    FetchContent_Declare(zydis
        URL      https://github.com/zyantific/zydis/archive/9bfadd6a55fc92dbd37fa3ba089bf8b36622df4f.tar.gz
        URL_HASH SHA256=2c168a83fc9f67c5abec287fde50d2b016757f159db8bf46aac138126d504edc
    )
    set(ZYAN_ZYCORE_PATH "${zycore_SOURCE_DIR}" CACHE PATH "" FORCE)
    set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
    set(ZYAN_FORCE_ASSERTS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(zydis)
    target_link_libraries(ck3_bridge PRIVATE Zydis)
    # Keep the build dir out of Zydis's assert strings.
    foreach(_zt Zydis Zycore)
        target_compile_options(${_zt} PRIVATE -fmacro-prefix-map=${FETCHCONTENT_BASE_DIR}/=)
    endforeach()

    # Vendored headers present (research checkout): they must equal the
    # fetched ones, or the two build modes compile different decoders.
    file(GLOB_RECURSE _vend_hdrs RELATIVE "${ZYDIS_PREFIX}/include"
        "${ZYDIS_PREFIX}/include/*.h")
    set(_skew "")
    foreach(_h IN LISTS _vend_hdrs)
        if(_h MATCHES "^Zycore/")
            set(_up "${zycore_SOURCE_DIR}/include/${_h}")
        else()
            set(_up "${zydis_SOURCE_DIR}/include/${_h}")
        endif()
        file(MD5 "${ZYDIS_PREFIX}/include/${_h}" _a)
        set(_b "missing")
        if(EXISTS "${_up}")
            file(MD5 "${_up}" _b)
        endif()
        if(NOT _a STREQUAL _b)
            list(APPEND _skew "${_h}")
        endif()
    endforeach()
    if(_skew)
        message(FATAL_ERROR "third_party/zydis/include differs from the pinned "
            "Zydis source in: ${_skew}. Update the pin or the vendored set.")
    endif()
endif()

# Optional: abort bridge init on BuildID mismatch (no signature fallback)
option(CK3_STRICT_BUILD_ID "Hard-fail on BuildID mismatch instead of falling back to signatures" OFF)
if(CK3_STRICT_BUILD_ID)
    target_compile_definitions(ck3_bridge PRIVATE CK3_STRICT_BUILD_ID)
endif()

# _GNU_SOURCE for SIGEV_THREAD_ID, sigev_notify_thread_id, gettid()
target_compile_definitions(ck3_bridge PRIVATE _GNU_SOURCE)

# Link librt for timer_create/timer_settime (SIGALRM watchdog)
target_link_libraries(ck3_bridge PRIVATE rt)

# Optimization: we want the hook functions to be fast
target_compile_options(ck3_bridge PRIVATE -O2 -Wall -Wextra)

# rc2 B2: debug verbs, compiled out unless ON: memory_probe, resolve_global,
# inject_mouse_click, select_observe_default, execute_console_command,
# set_cb_selector_override (their registry entries AND handler bodies), plus
# every process VA in a reply. Default OFF; research builds pass
# -DCK3_BRIDGE_DEBUG_VERBS=ON. test_debug_verbs pins the set; the
# raw_pointer_inputs / no_va_output censuses below walk the handlers.
option(CK3_BRIDGE_DEBUG_VERBS "Compile in the debug IPC verbs" OFF)
# B9 defence in depth: every RapidJSON Parse without explicit flags is iterative.
target_compile_definitions(ck3_bridge PRIVATE RAPIDJSON_PARSE_DEFAULT_FLAGS=kParseIterativeFlag)
if(CK3_BRIDGE_DEBUG_VERBS)
    target_compile_definitions(ck3_bridge PRIVATE CK3_BRIDGE_DEBUG_VERBS)
endif()

# Hardening. -U first: some toolchains predefine _FORTIFY_SOURCE.
target_compile_options(ck3_bridge PRIVATE
    -fstack-protector-strong -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -fcf-protection
    # Keep the build host's paths out of __FILE__ (assert strings).
    -fmacro-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}/=bridge/
)
target_link_options(ck3_bridge PRIVATE "LINKER:-z,relro,-z,now")

# Exports: only the exec-family interposers listed in bridge.version.
# Everything else is hidden at compile time; static archives (Zydis) at link.
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/bridge.version)
    message(FATAL_ERROR "bridge/bridge.version is missing (linker version script)")
endif()
set_target_properties(ck3_bridge PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/bridge.version
)
# The interposers must stay default-visible for the version script to export them.
set_source_files_properties(src/launcher_bypass.cpp PROPERTIES
    COMPILE_OPTIONS -fvisibility=default)
target_link_options(ck3_bridge PRIVATE
    "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/bridge.version"
    "LINKER:--exclude-libs,ALL"
)

# Don't link against standard C++ runtime startup — we use constructor/destructor attrs
set_target_properties(ck3_bridge PROPERTIES
    OUTPUT_NAME "ck3_bridge"
)

# Install target
install(TARGETS ck3_bridge LIBRARY DESTINATION lib)

# -----------------------------------------------------------------------------
# i386 no-op stub (ck-dq7ym.1)
#
# A tiny 32-bit .so so one LD_PRELOAD list satisfies both Steam's 32-bit shims
# and the 64-bit ck3 binary without ELFCLASS-mismatch warnings. One gcc -m32
# call; -nostdlib + a hand-rolled .init_array, so no 32-bit glibc is needed.
# Built only if the source exists and the compiler can target -m32, so 'all'
# stays green without multilib.
# -----------------------------------------------------------------------------
set(BRIDGE_STUB_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/src/bridge_stub.c)
set(BRIDGE_STUB_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/libck3_bridge_stub.so)
set(BRIDGE_STUB_FLAGS -m32 -shared -nostdlib -fPIC -O2 -Wall -Wextra)
set(CK3_BRIDGE_STUB OFF)
if(EXISTS ${BRIDGE_STUB_SOURCE})
    # Probe with the real command: -m32 support is the only unknown.
    execute_process(
        COMMAND ${CMAKE_C_COMPILER} ${BRIDGE_STUB_FLAGS} ${BRIDGE_STUB_SOURCE}
                -o ${CMAKE_CURRENT_BINARY_DIR}/m32_probe.so
        RESULT_VARIABLE _m32_rc OUTPUT_QUIET ERROR_QUIET)
    file(REMOVE ${CMAKE_CURRENT_BINARY_DIR}/m32_probe.so)
    if(_m32_rc EQUAL 0)
        set(CK3_BRIDGE_STUB ON)
    else()
        message(STATUS "ck3_bridge_stub: skipped, compiler cannot build -m32")
    endif()
else()
    message(STATUS "ck3_bridge_stub: skipped, src/bridge_stub.c absent")
endif()

if(CK3_BRIDGE_STUB)
    add_custom_command(
        OUTPUT ${BRIDGE_STUB_OUTPUT}
        COMMAND ${CMAKE_C_COMPILER} ${BRIDGE_STUB_FLAGS}
                ${BRIDGE_STUB_SOURCE} -o ${BRIDGE_STUB_OUTPUT}
        DEPENDS ${BRIDGE_STUB_SOURCE}
        COMMENT "Building i386 stub libck3_bridge_stub.so (ck-dq7ym.1)"
        VERBATIM
    )
    add_custom_target(ck3_bridge_stub ALL DEPENDS ${BRIDGE_STUB_OUTPUT})
    install(FILES ${BRIDGE_STUB_OUTPUT} DESTINATION lib)
endif()

# -----------------------------------------------------------------------------
# Unit tests (opt-in). Currently covers ck-8xzy regression for json_get_string.
# Enable with: cmake -DBUILD_BRIDGE_TESTS=ON ..
# -----------------------------------------------------------------------------
option(BUILD_BRIDGE_TESTS "Build standalone unit tests for bridge helpers" OFF)
if(BUILD_BRIDGE_TESTS)
    # THE STUB SET FOLLOWS THE DEPENDENCY, NOT THE TARGET — and nothing else in
    # this tree writes that down, which cost the merge queue two link failures
    # and one confident wrong diagnosis. A host target's .cpp must DEFINE:
    #   linking session.cpp            -> the six controlled-character globals
    #                                     (g_logfile, g_controlled_mutex,
    #                                     g_controlled_chars, g_controlled_primary,
    #                                     g_controlled_heir, g_agent_mode)
    #   linking world_epoch.cpp too    -> ALSO g_base_addr, g_valid_ranges and
    #                                     g_ranges_mutex, for its guarded reads
    # Since ck-6kvq.5 the first implies the second, because session.cpp reaches
    # world_identity.cpp and that reaches world_epoch.cpp. So adding this
    # variable to a target is only HALF the fix: without the second group the
    # error does not go away, it MOVES to `undefined reference to g_base_addr`.
    # Discoverable only by link failure, which is why it is written here.
    #
    # ck-6kvq.5: EVERY host target that links src/session.cpp needs these.
    # session.cpp takes the RESUME decision with ck-9fc3's judge_same_save()
    # (ONE identity rule, not a second copy of it), which pulls in the
    # engine-free TUs below. Named once so a new target cannot link session.cpp
    # and silently fail to link — or worse, so a NEGATIVE CONTROL cannot stop
    # compiling, since a compile failure reads as "the control failed", which is
    # what the control is supposed to do.
    # ck-81su: world_identity.cpp now resolves the BOARD's carrier on the same
    # declaration it resolves the personality quarantine on (one judge, one
    # decision), so every target that links it also links the board. Named here
    # for the same reason the list exists at all: a new target must not be able
    # to link world_identity.cpp and silently fail to link.
    #
    # THE COST OF THAT, AND THE TELL. This pulls episode_policy.cpp into targets
    # with nothing to do with the board — acceptance_save_identity and
    # acceptance_world_reset among them. Harmless TODAY, and checked rather than
    # assumed: neither of those references episode_policy at all, so neither can
    # read what another lane left in it. What makes it worth a comment is that
    # episode_policy() has set() and init_from_env() but NO reset_for_test(), so
    # the moment a SECOND host target starts READING the policy, the first one to
    # write it wins for the rest of the process. It would present as a lane that
    # passes or fails depending on the order lanes run in main() — which is
    # exactly the symptom nobody attributes to a singleton. If that ever
    # happens, the fix is a reset seam on the holder, not a change here.
    set(SESSION_HOST_SOURCES
        src/session.cpp
        src/world_identity.cpp
        src/world_epoch.cpp
        src/personality_world_reset.cpp
        src/personality_persist.cpp
        src/board.cpp
        src/board_persist.cpp
        src/episode_policy.cpp
        src/action_args.cpp
    )
    # ck-ijfa: the clock-rate meter. PURE — wall time is passed in, so a year of
    # episode runs in a microsecond and no CK3 is needed.
    add_executable(test_clock_rate
        test/test_clock_rate.cpp
        src/clock_rate.cpp
    )
    target_include_directories(test_clock_rate PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_link_libraries(test_clock_rate PRIVATE Threads::Threads)
    target_compile_options(test_clock_rate PRIVATE -O2 -Wall -Wextra)

    add_executable(test_json_helpers
        test/test_json_helpers.cpp
        src/json_helpers.cpp
    )
    target_include_directories(test_json_helpers PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_link_libraries(test_json_helpers PRIVATE Threads::Threads)
    target_compile_options(test_json_helpers PRIVATE -O2 -Wall -Wextra)

    # ck-zvu5l.3: parser for `<effect> = { <key>=<val> ... }` blocks.
    add_executable(test_script_block_parser
        test/test_script_block_parser.cpp
        src/script_block_parser.cpp
    )
    target_include_directories(test_script_block_parser PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_options(test_script_block_parser PRIVATE -O2 -Wall -Wextra)

    # ck-qn03 (R12): the instance naming rules, compiled standalone. The test
    # is a CROSS-CHECK, not a second opinion: `scripts/test_multi_instance.py`
    # runs this binary and compares its answers, key for key, against the
    # python authority and the shell helper. Three transcriptions that agree
    # are a contract; three that drift are the bug this bead removes.
    add_executable(test_instance_paths
        test/test_instance_paths.cpp
        src/instance_paths.cpp
    )
    target_include_directories(test_instance_paths PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_options(test_instance_paths PRIVATE -O2 -Wall -Wextra)

    # ck-05mg: CK3's real calendar (no twelve-30-day-month rendering).
    add_executable(test_game_date
        test/test_game_date.cpp
    )
    target_include_directories(test_game_date PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets
    )
    target_compile_options(test_game_date PRIVATE -O2 -Wall -Wextra)

    # ck-k57a: the typed speed read. HEADER-ONLY and engine-free, so the memory
    # fault that used to present as a legal "speed 1" is reachable host-side.
    add_executable(test_speed_read
        test/test_speed_read.cpp
    )
    target_include_directories(test_speed_read PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_options(test_speed_read PRIVATE -O2 -Wall -Wextra)

    # ck-k57a: the handler's OWN emission, on a host. `scripts/
    # acceptance_speed_fault.py` runs this to build its fixtures, so the wire
    # bytes the acceptance judges are the bridge's rather than a python model
    # of them. Built TWICE — the second with the pre-fix arithmetic restored,
    # as the acceptance's negative control.
    add_executable(speed_read_probe
        test/speed_read_probe.cpp
    )
    target_include_directories(speed_read_probe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_options(speed_read_probe PRIVATE -O2 -Wall -Wextra)

    add_executable(speed_read_probe_prefix_control
        test/speed_read_probe.cpp
    )
    target_include_directories(speed_read_probe_prefix_control PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_definitions(speed_read_probe_prefix_control PRIVATE
        CK3_SPEED_PREFIX_CONTROL=1)
    target_compile_options(speed_read_probe_prefix_control PRIVATE -O2 -Wall -Wextra)

    # ck-cyhr: the FIRST coverage bridge/src/deferred_drain.cpp has ever had.
    # The probe links the REAL coordinator against a fake engine (the engine
    # surface it needs is six symbols), so what is exercised is the shipping
    # pause-transparency pulse and not a model of it. Driven by
    # `scripts/acceptance_pulse_restore.py`, which ALSO compiles it a second
    # time against the PRE-FIX deferred_drain.cpp staged from a pinned blob sha
    # — that second build is the acceptance's negative control and is deliberately
    # NOT a CMake target, because a control pinned by a ref rather than a blob
    # would grade the fix against the fix the moment this branch merges.
    add_executable(pulse_restore_probe
        test/pulse_restore_probe.cpp
        src/deferred_drain.cpp
        # ck-p2sb: deferred_drain.cpp INCREMENTS the two refusal counters and no
        # longer DEFINES them (they are a leaf TU so that host targets compiling
        # dispatch_router.cpp can link without dragging in engine symbols). Any
        # target that compiles deferred_drain.cpp OR dispatch_router.cpp needs
        # this file.
        src/deferred_counters.cpp
    )
    target_include_directories(pulse_restore_probe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets
    )
    target_link_libraries(pulse_restore_probe PRIVATE Threads::Threads)
    target_compile_options(pulse_restore_probe PRIVATE -O1 -Wall -Wextra)

    # ck-nkyc leg (e). The strictness witness's fixtures come from the BRIDGE's
    # own code, not from a python model of it — and the negative control is the
    # PRE-FIX rule compiled from the same source, so a control that stops
    # failing is a control whose premise the shipped code has adopted.
    add_executable(build_id_witness_probe
        test/build_id_witness_probe.cpp
        src/build_id_witness.cpp
    )
    target_include_directories(build_id_witness_probe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_options(build_id_witness_probe PRIVATE -O2 -Wall -Wextra)

    add_executable(build_id_witness_probe_prefix_control
        test/build_id_witness_probe.cpp
        src/build_id_witness.cpp
    )
    target_include_directories(build_id_witness_probe_prefix_control PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_definitions(build_id_witness_probe_prefix_control PRIVATE
        CK3_WITNESS_PREFIX_CONTROL=1)
    target_compile_options(build_id_witness_probe_prefix_control PRIVATE -O2 -Wall -Wextra)

    # M1: session registry + ownership table.
    # ck-6kvq.5: session.cpp now takes the RESUME decision with ck-9fc3's
    # judge_same_save() — one identity rule, not a second copy of it — so the
    # host target links world_identity.cpp and the engine-free TUs it needs.
    add_executable(test_session_registry
        test/test_session_registry.cpp
        ${SESSION_HOST_SOURCES}
    )

    # ck-swap: the cap-full row only exists with the roster FILLED, and a test
    # that does not fill it never runs the code it grades.
    add_executable(swap_character_probe
        test/swap_character_probe.cpp
        ${SESSION_HOST_SOURCES}
    )
    target_include_directories(swap_character_probe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_link_libraries(swap_character_probe PRIVATE Threads::Threads)
    target_compile_options(swap_character_probe PRIVATE -O2 -Wall -Wextra)

    # ck-handoff-swap: the succession handoff at a full roster — the case that
    # spun check_controlled_handoff 2,574 times because claim-then-release cannot
    # work while the corpse still occupies the slot it needs.
    # ck-qv51: the roster-divergence rollback must not destroy a real last_steered.
    # Same source set as handoff_swap_probe — the registry plus its host deps — since
    # the claim/release/undo sequence under test is entirely registry-level.
    # ck-j6lk: the claim pin must key on CLAIMS, not on being a control session.
    # Exercises claims_ever across claim, release and swap on the REAL registry, so
    # the "claimed then released" seat and the "never claimed" helper are separated
    # by the field rather than by a fixture's say-so. Same source set as
    # rollback_state_probe: the registry plus its host deps, no game.
    # ck-j6lk second pass: GRADE THE LEDGER'S REPORT. The first pass proved the
    # registry destroyed the non-participants and shakedown 3 still sealed invalid,
    # because unavailability's note_session_ended KEEPS the record accruing. Links the
    # REAL unavailability.cpp and reads snapshot() — the thing the bundle is built
    # from — so the discriminating pair is a pair of reports, not of registry states.
    add_executable(unavail_forget_probe
        test/unavail_forget_probe.cpp
        src/unavailability.cpp
    )
    target_include_directories(unavail_forget_probe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_compile_options(unavail_forget_probe PRIVATE -O2 -Wall -Wextra)

    add_executable(claimless_detach_probe
        test/claimless_detach_probe.cpp
        ${SESSION_HOST_SOURCES}
    )
    target_include_directories(claimless_detach_probe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_link_libraries(claimless_detach_probe PRIVATE Threads::Threads)
    target_compile_options(claimless_detach_probe PRIVATE -O2 -Wall -Wextra)

    add_executable(rollback_state_probe
        test/rollback_state_probe.cpp
        ${SESSION_HOST_SOURCES}
    )
    target_include_directories(rollback_state_probe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_link_libraries(rollback_state_probe PRIVATE Threads::Threads)
    target_compile_options(rollback_state_probe PRIVATE -O2 -Wall -Wextra)

    # ck-zs2x: the aiwatch ring DROPS on full rather than overwriting, so a wrap
    # leaves no trace in the data and `dropped` is the only evidence. This probe
    # measures the real capacity (kSlots-1), the push at which loss begins, and
    # whether the loss is VISIBLE on a line of its own. Header-only: the watch is all
    # inline, so it needs no SESSION_HOST_SOURCES and links against nothing.
    add_executable(ai_watch_drop_probe
        test/ai_watch_drop_probe.cpp
    )
    target_include_directories(ai_watch_drop_probe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_compile_options(ai_watch_drop_probe PRIVATE -O2 -Wall -Wextra)

    add_executable(handoff_swap_probe
        test/handoff_swap_probe.cpp
        ${SESSION_HOST_SOURCES}
    )
    target_include_directories(handoff_swap_probe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_link_libraries(handoff_swap_probe PRIVATE Threads::Threads)
    target_compile_options(handoff_swap_probe PRIVATE -O2 -Wall -Wextra)
    target_include_directories(test_session_registry PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_link_libraries(test_session_registry PRIVATE Threads::Threads)
    target_compile_options(test_session_registry PRIVATE -O2 -Wall -Wextra)

    # ck-6kvq.11 (R2): the N=6 ownership ACCEPTANCE. Same real session.cpp as
    # test_session_registry, but it reports every check instead of aborting on
    # the first, and emits --json so scripts/acceptance_n6_ownership.py can
    # compare the elected primary and the roster bytes ACROSS FRESH PROCESSES —
    # the one determinism claim a single in-process run cannot make.
    add_executable(acceptance_n6_ownership
        test/acceptance_n6_ownership.cpp
        ${SESSION_HOST_SOURCES}
    )
    target_include_directories(acceptance_n6_ownership PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_link_libraries(acceptance_n6_ownership PRIVATE Threads::Threads)
    target_compile_options(acceptance_n6_ownership PRIVATE -O2 -Wall -Wextra)

    # ck-xqol: IPC client-slot LIFECYCLE — release with an attached session,
    # double release, release racing a handshake, and the never-constructed
    # slot table that produced the production SIGSEGV. Links the real session
    # registry, so a leaked or double-destroyed session is observable.
    add_executable(test_client_slot
        test/test_client_slot.cpp
        src/client_slot.cpp
        ${SESSION_HOST_SOURCES}
    )
    target_include_directories(test_client_slot PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_link_libraries(test_client_slot PRIVATE Threads::Threads)
    target_compile_options(test_client_slot PRIVATE -O2 -Wall -Wextra)

    # ck-xqol NEGATIVE CONTROL. Identical source, built with the pre-fix guard
    # restored (-DCK3_SLOT_LEGACY_FD_GUARD). It MUST fail: the never-constructed
    # slot SIGSEGVs in the same statement as the production core, and the
    # handshake race orphans a session. Never run as part of a green suite —
    # it is the proof that the green suite is measuring something.
    add_executable(test_client_slot_negative_control
        test/test_client_slot.cpp
        src/client_slot.cpp
        ${SESSION_HOST_SOURCES}
    )
    target_include_directories(test_client_slot_negative_control PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_compile_definitions(test_client_slot_negative_control PRIVATE
        CK3_SLOT_LEGACY_FD_GUARD=1)
    target_link_libraries(test_client_slot_negative_control PRIVATE Threads::Threads)
    target_compile_options(test_client_slot_negative_control PRIVATE -O2 -Wall -Wextra)

    # ck-6kvq.8 (R1a): buffered, polarity-driven flush scheduler. Pure state
    # machine — no engine, no game, no stub globals needed.
    add_executable(test_flush_scheduler
        test/test_flush_scheduler.cpp
        src/flush_scheduler.cpp
    )
    target_include_directories(test_flush_scheduler PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(test_flush_scheduler PRIVATE Threads::Threads)
    target_compile_options(test_flush_scheduler PRIVATE -O2 -Wall -Wextra)

    # ck-6kvq.15: the params-fallback resolver. Engine-free by design so the
    # wire shapes a real client sends can be asserted on a host — the bug it
    # fixes was invisible to every existing test because they all framed their
    # arguments the one way the broken read happened to handle.
    # ck-fvms.1: what the observer's meter read actually waits for. PURE —
    # wall time is passed in, so a whole episode's poll series runs in a
    # microsecond and no CK3 is needed.
    add_executable(meter_lane_latency_probe
        test/meter_lane_latency_probe.cpp
        src/flush_scheduler.cpp
    )
    target_include_directories(meter_lane_latency_probe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(meter_lane_latency_probe PRIVATE Threads::Threads)
    target_compile_options(meter_lane_latency_probe PRIVATE -O2 -Wall -Wextra)

    add_executable(test_action_args
        test/test_action_args.cpp
        src/action_args.cpp
    )
    target_include_directories(test_action_args PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_compile_options(test_action_args PRIVATE -O2 -Wall -Wextra)

    # ck-6kvq.13 (R6): the decision-budget governor's ENFORCING half. Pure by
    # construction — no engine, no game, no stub global — because "is this agent
    # out of budget" must be provable without a running game. Also carries the
    # --replay mode scripts/test_budget_governor.py drives a recorded transcript
    # through, so there is exactly one implementation of the rule.
    add_executable(test_budget_ledger
        test/test_budget_ledger.cpp
        src/budget_ledger.cpp
    )
    target_include_directories(test_budget_ledger PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_link_libraries(test_budget_ledger PRIVATE Threads::Threads)
    target_compile_options(test_budget_ledger PRIVATE -O2 -Wall -Wextra)

    # ck-6kvq.9 (R1b): the apply + ticket layer. Also engine-free — the plan is
    # applied against a FAKE FlushEffects, which is the whole point of the
    # scheduler/apply split.
    add_executable(test_pipeline
        test/test_pipeline.cpp
        src/flush_scheduler.cpp
        src/ticket_table.cpp
        src/flush_apply.cpp
    )
    target_include_directories(test_pipeline PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(test_pipeline PRIVATE Threads::Threads)
    target_compile_options(test_pipeline PRIVATE -O2 -Wall -Wextra)

    # ck-6kvq.18: the pipeline egress frame assembly. Header-only and pure, so
    # the splice that rewrites EVERY pipeline reply can be parsed and proved on
    # a host WITHOUT pulling the rest of the pipeline in — which is why this
    # target exists and is still the right shape for it.
    #
    # ck-1var CORRECTS WHAT USED TO BE THE REST OF THIS SENTENCE: pipeline.cpp DOES link on a host —
    # measured by ck-1var, which drives the real pipeline_note_episode_open and
    # reads the real pipeline_emit_state against 21 ordinary host TUs, the
    # generated dispatch-handler stubs test_release_handler already uses, and a
    # small engine boundary (bridge/test/clock_release_probe.cpp). The sentence
    # that used to stand here said it could not, and that belief is WHY ck-8o5j's
    # clock release went unwitnessed through two beads: nobody tried, because the
    # build file said not to. A build file states TARGETS; a comment in it states
    # a BELIEF, and a reassurance is no more a control than a warning is.
    add_executable(test_pipeline_frames test/test_pipeline_frames.cpp)
    target_include_directories(test_pipeline_frames PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_compile_options(test_pipeline_frames PRIVATE -O2 -Wall -Wextra)

    # ck-6kvq.18 NEGATIVE CONTROL. Same source with
    # -DCK3_PIPELINE_FRAME_UNTYPED=1: the frame closes with no `type`, as it
    # did before. It MUST fail — that untypeable frame is precisely what a
    # request_id-less client could neither route nor skip-list.
    add_executable(test_pipeline_frames_negative_control
        test/test_pipeline_frames.cpp)
    target_include_directories(test_pipeline_frames_negative_control PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_compile_definitions(test_pipeline_frames_negative_control PRIVATE
        CK3_PIPELINE_FRAME_UNTYPED=1)
    target_compile_options(test_pipeline_frames_negative_control PRIVATE
        -O2 -Wall -Wextra)

    # ck-6kvq.4 / ck-1ek9: THE MODE POLICY TABLE. Pure — no engine, no globals,
    # no getenv — so the whole of what `null` MEANS is host-testable with no CK3
    # binary. This is the half of ck-6kvq.4 that never merged (WIP 425c20b);
    # ck-1ek9 recovered it.
    add_executable(test_pipeline_mode
        test/test_pipeline_mode.cpp
        src/pipeline_mode.cpp
        src/flush_scheduler.cpp
    )
    target_include_directories(test_pipeline_mode PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_compile_options(test_pipeline_mode PRIVATE -O2 -Wall -Wextra)

    # ck-6kvq.4 NEGATIVE CONTROL. Same two sources with
    # -DCK3_NULL_MODE_LEGACY_POLICY=1, which reverts the five predicates to the
    # policy the bridge ACTUALLY HAD before this bead: `pipeline_active()` was
    # `mode != Legacy`, and nothing consulted the mode about control sessions or
    # the checkpoint gate at all. It MUST fail. A test of a policy table would
    # otherwise pass by restating the table it is testing.
    add_executable(test_pipeline_mode_negative_control
        test/test_pipeline_mode.cpp
        src/pipeline_mode.cpp
        src/flush_scheduler.cpp
    )
    target_include_directories(test_pipeline_mode_negative_control PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_compile_definitions(test_pipeline_mode_negative_control PRIVATE
        CK3_NULL_MODE_LEGACY_POLICY=1)
    target_compile_options(test_pipeline_mode_negative_control PRIVATE
        -O2 -Wall -Wextra)

    # ck-6kvq.19: the drain correlation. Engine-free so the hand-off/settle
    # protocol can be driven on a host with no game — which is exactly where
    # the bug hid: the pre-fix correlation keyed on the client's OPTIONAL
    # request_id, and every existing test happened to send one.
    add_executable(test_drain_registry
        test/test_drain_registry.cpp
        src/drain_registry.cpp
    )
    target_include_directories(test_drain_registry PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_link_libraries(test_drain_registry PRIVATE Threads::Threads)
    target_compile_options(test_drain_registry PRIVATE -O2 -Wall -Wextra)

    # ck-6kvq.19 NEGATIVE CONTROL. Identical sources, built with
    # -DCK3_DRAIN_LEGACY_RID_KEY=1: the drain envelope loses its bridge-minted
    # ticket tag and correlation goes back to the client's optional, non-unique
    # request_id. It MUST fail — a rid-less call never settles and leaks its
    # payload, and two calls sharing one request_id collide. Never run as part
    # of a green suite; it is the proof that the green suite measures something.
    add_executable(test_drain_registry_negative_control
        test/test_drain_registry.cpp
        src/drain_registry.cpp
    )
    target_include_directories(test_drain_registry_negative_control PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_compile_definitions(test_drain_registry_negative_control PRIVATE
        CK3_DRAIN_LEGACY_RID_KEY=1)
    target_link_libraries(test_drain_registry_negative_control PRIVATE Threads::Threads)
    target_compile_options(test_drain_registry_negative_control PRIVATE -O2 -Wall -Wextra)

    # ck-6kvq.10 (R1c): the pulse ledger. Engine-free — memory goes through an
    # abstract PulseMemory, so the crash path is reproduced with a real
    # siglongjmp and no CK3 binary.
    add_executable(test_pulse_ledger
        test/test_pulse_ledger.cpp
        src/pulse_ledger.cpp
    )
    target_include_directories(test_pulse_ledger PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(test_pulse_ledger PRIVATE Threads::Threads)
    target_compile_options(test_pulse_ledger PRIVATE -O2 -Wall -Wextra)

    # ck-sozz: release_character's HANDLER ORDERING.
    #
    # The bug was a composition: two individually-correct guards in the wrong
    # order inside do_release_character, so every release of a session-owned
    # character answered not_in_roster AFTER having already dropped the claim,
    # and skipped the cleanup half (personality lock, council lock, native AI).
    # test_session_registry could not see it — it drives SessionRegistry
    # directly, and the registry half is correct. So this test links the REAL
    # dispatch_router.cpp and calls the real handler.
    #
    # dispatch_router.cpp also carries the master dispatch table, which names
    # ~150 handlers that live in other (engine-bound) TUs. They are supplied as
    # WEAK no-ops generated from the header; the handlers dispatch_router.cpp
    # defines itself provide strong definitions and win the link.
    find_program(CK3_PYTHON3 NAMES python3 REQUIRED)
    set(DISPATCH_HANDLER_STUBS ${CMAKE_CURRENT_BINARY_DIR}/dispatch_handler_stubs.cpp)
    add_custom_command(
        OUTPUT ${DISPATCH_HANDLER_STUBS}
        COMMAND ${CK3_PYTHON3}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/gen_dispatch_handler_stubs.py
                ${CMAKE_CURRENT_SOURCE_DIR}/include/action_dispatch_internal.h
                ${DISPATCH_HANDLER_STUBS}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test/gen_dispatch_handler_stubs.py
                ${CMAKE_CURRENT_SOURCE_DIR}/include/action_dispatch_internal.h
        COMMENT "Generating weak dispatch-handler stubs for test_release_handler (ck-sozz)"
        VERBATIM
    )

    set(RELEASE_HANDLER_SOURCES
        test/test_release_handler.cpp
        src/dispatch_router.cpp
        ${SESSION_HOST_SOURCES}
        src/json_helpers.cpp
        src/budget_ledger.cpp
        src/instance_paths.cpp
        # ck-1ek9: dispatch_router.cpp's observe-mode gate now asks the mode
        # policy whether a null-mode observer may issue a clock verb, so it
        # needs pipeline_mode()/mode_admits_observer_clock_verbs() at link time.
        # ANY host target that compiles dispatch_router.cpp needs this TU — see
        # the note beside src/pipeline_mode.cpp in the library list above.
        src/pipeline_mode.cpp
        # ck-p2sb: dispatch_router.cpp's get_bridge_state reports the deferred
        # coordinator's two refusal counts, so this target needs their leaf TU
        # at link time — the same rule, one symbol later.
        src/deferred_counters.cpp
        ${DISPATCH_HANDLER_STUBS}
    )
    set(RELEASE_HANDLER_INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )

    add_executable(test_release_handler ${RELEASE_HANDLER_SOURCES})
    target_include_directories(test_release_handler PRIVATE ${RELEASE_HANDLER_INCLUDES})
    target_link_libraries(test_release_handler PRIVATE Threads::Threads)
    target_compile_options(test_release_handler PRIVATE -O2 -Wall -Wextra)

    # ck-sozz NEGATIVE CONTROL. Identical source, with dispatch_router.cpp built
    # -DCK3_RELEASE_LEGACY_ORDER: the ownership release goes back above the
    # roster guard, and set_controlled_character's AI-disable goes back above
    # its own roster check. It MUST fail, reproducing the live 2026-09-10
    # symptom verbatim ({"success":false,"reason":"not_in_roster"} on a claim
    # that has in fact already been released). Never run as part of a green
    # suite — it is the proof that the green suite measures something.
    add_executable(test_release_handler_negative_control ${RELEASE_HANDLER_SOURCES})
    target_include_directories(test_release_handler_negative_control PRIVATE
        ${RELEASE_HANDLER_INCLUDES})
    target_compile_definitions(test_release_handler_negative_control PRIVATE
        CK3_RELEASE_LEGACY_ORDER=1)
    target_link_libraries(test_release_handler_negative_control PRIVATE Threads::Threads)
    target_compile_options(test_release_handler_negative_control PRIVATE -O2 -Wall -Wextra)

    # rc2 B2: the debug verbs are compiled out and answer unavailable_in_release.
    # Same sources as test_release_handler (the REAL dispatch_router.cpp). The
    # negative control builds it with CK3_BRIDGE_DEBUG_VERBS and MUST fail.
    set(DEBUG_VERBS_SOURCES ${RELEASE_HANDLER_SOURCES})
    list(REMOVE_ITEM DEBUG_VERBS_SOURCES test/test_release_handler.cpp)
    list(APPEND DEBUG_VERBS_SOURCES test/test_debug_verbs.cpp)
    add_executable(test_debug_verbs ${DEBUG_VERBS_SOURCES})
    target_include_directories(test_debug_verbs PRIVATE ${RELEASE_HANDLER_INCLUDES})
    target_link_libraries(test_debug_verbs PRIVATE Threads::Threads)
    target_compile_options(test_debug_verbs PRIVATE -O2 -Wall -Wextra)
    add_executable(test_debug_verbs_negative_control ${DEBUG_VERBS_SOURCES})
    target_include_directories(test_debug_verbs_negative_control PRIVATE
        ${RELEASE_HANDLER_INCLUDES})
    target_compile_definitions(test_debug_verbs_negative_control PRIVATE
        CK3_BRIDGE_DEBUG_VERBS)
    target_link_libraries(test_debug_verbs_negative_control PRIVATE Threads::Threads)
    target_compile_options(test_debug_verbs_negative_control PRIVATE -O2 -Wall -Wextra)

    # rc2 B1 + B9: the REAL ipc_server.cpp on a real socket — the peer-uid
    # check is wired at accept(), the reader caps line length and nesting.
    # Inherits ck3_bridge's compile definitions, so it parses with the
    # library's RapidJSON default flags.
    get_target_property(_ck3_bridge_defs ck3_bridge COMPILE_DEFINITIONS)
    add_executable(test_ipc_wiring
        test/test_ipc_wiring.cpp
        src/ipc_server.cpp
        ${SESSION_HOST_SOURCES}
        src/instance_paths.cpp
        src/pipeline_mode.cpp
        src/json_helpers.cpp
        src/client_slot.cpp
        src/log_archive.cpp
        src/build_id_witness.cpp
    )
    add_dependencies(test_ipc_wiring gen_provenance)
    target_include_directories(test_ipc_wiring PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
        ${CMAKE_CURRENT_BINARY_DIR}
    )
    if(_ck3_bridge_defs)
        target_compile_definitions(test_ipc_wiring PRIVATE ${_ck3_bridge_defs})
    endif()
    target_link_libraries(test_ipc_wiring PRIVATE Threads::Threads)
    target_compile_options(test_ipc_wiring PRIVATE -O2 -Wall -Wextra)

    # rc2 B6: the succession handoff never binds the human player.
    add_executable(test_handoff_player
        test/test_handoff_player.cpp
        ${SESSION_HOST_SOURCES}
    )
    target_include_directories(test_handoff_player PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_link_libraries(test_handoff_player PRIVATE Threads::Threads)
    target_compile_options(test_handoff_player PRIVATE -O2 -Wall -Wextra)

    # ck-6kvq.10 (R3 as REVERSED): unavailability accounting + the episode
    # policy. Pure, no engine deps, no CK3 binary.
    add_executable(test_unavailability
        test/test_unavailability.cpp
        src/unavailability.cpp
        src/episode_policy.cpp
    )
    target_include_directories(test_unavailability PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        # ck-2ngg: exclusion_rule.h computes the horizon span through
        # ck3::game_date (the ENGINE's calendar, ck-05mg), which lives beside
        # the offsets header. Still engine-FREE: constants only, no reads.
        ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets
    )
    target_link_libraries(test_unavailability PRIVATE Threads::Threads)
    target_compile_options(test_unavailability PRIVATE -O2 -Wall -Wextra)
    # ck-2ngg ACCEPTANCE: X is a RULE (5% of the DECLARED horizon), not the
    # constant 365. Two targets from ONE source, the ck-3p1x/speed_read_probe
    # pattern: the product lane links the shipped modules; the NEGATIVE CONTROL
    # links the REAL pre-ck-2ngg bytes pulled out of git history into
    # scripts/fixtures/exclusion_rule/pre_2ngg/, and must FAIL lanes B and D.
    # Real bytes rather than a re-implementation, because a control that
    # re-implements the defect proves only that the re-implementation is wrong.
    # Driven by scripts/acceptance_exclusion_fraction.py.
    add_executable(acceptance_exclusion_fraction
        test/acceptance_exclusion_fraction.cpp
        src/unavailability.cpp
        src/episode_policy.cpp
    )
    target_include_directories(acceptance_exclusion_fraction PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets
    )
    target_link_libraries(acceptance_exclusion_fraction PRIVATE Threads::Threads)
    target_compile_options(acceptance_exclusion_fraction PRIVATE -O2 -Wall -Wextra)

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/fixtures/exclusion_rule/pre_2ngg)
    set(EXCLUSION_PREFIX_DIR
        ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/fixtures/exclusion_rule/pre_2ngg)
    add_executable(acceptance_exclusion_fraction_negative_control
        test/acceptance_exclusion_fraction.cpp
        ${EXCLUSION_PREFIX_DIR}/unavailability.cpp
        ${EXCLUSION_PREFIX_DIR}/episode_policy.cpp
    )
    # THE FIXTURE DIRECTORY COMES FIRST so the pre-fix .cpp files resolve their
    # own pre-fix headers, never the shipped ones beside them.
    target_include_directories(acceptance_exclusion_fraction_negative_control
        BEFORE PRIVATE ${EXCLUSION_PREFIX_DIR})
    target_compile_definitions(acceptance_exclusion_fraction_negative_control
        PRIVATE CK3_EXCLUSION_PREFIX_CONTROL=1)
    target_link_libraries(acceptance_exclusion_fraction_negative_control
        PRIVATE Threads::Threads)
    target_compile_options(acceptance_exclusion_fraction_negative_control
        PRIVATE -O2 -Wall -Wextra)
else()
    message(STATUS "fixture dir absent; skipping acceptance_exclusion_fraction_negative_control")
endif()

    # ck-on4u: THE RESTING SPEED IS THE PRE-REGISTERED PIN. The flush planner's
    # restore step is the SECOND writer of the game speed — it reaches
    # game_set_speed through the effects vtable, so a grep for the function name
    # finds one call site while two paths write, and it logs no "Action:
    # set_speed" line. Both TUs are engine-free, so the question "what speed does
    # the plan's SetSpeed step carry?" is answerable on a host with no CK3.
    add_executable(acceptance_resting_speed
        test/acceptance_resting_speed.cpp
        src/flush_scheduler.cpp
        src/episode_policy.cpp
    )
    target_include_directories(acceptance_resting_speed PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets
    )
    target_link_libraries(acceptance_resting_speed PRIVATE Threads::Threads)
    target_compile_options(acceptance_resting_speed PRIVATE -O2 -Wall -Wextra)

    # THE NEGATIVE CONTROL: the same probe over the PRE-FIX BYTES, pinned by
    # blob SHA in scripts/fixtures/resting_speed/pre_on4u/. It must FAIL —
    # the planner rests at a hard-coded 3 there, whatever the episode declares.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/fixtures/resting_speed/pre_on4u)
    set(RESTING_SPEED_PREFIX_DIR
        ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/fixtures/resting_speed/pre_on4u)
    add_executable(acceptance_resting_speed_negative_control
        test/acceptance_resting_speed.cpp
        ${RESTING_SPEED_PREFIX_DIR}/flush_scheduler.cpp
        ${RESTING_SPEED_PREFIX_DIR}/episode_policy.cpp
    )
    # THE FIXTURE DIRECTORY COMES FIRST so the pre-fix .cpp files resolve their
    # own pre-fix headers, never the shipped ones beside them.
    target_include_directories(acceptance_resting_speed_negative_control
        BEFORE PRIVATE ${RESTING_SPEED_PREFIX_DIR})
    target_include_directories(acceptance_resting_speed_negative_control PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets)
    target_compile_definitions(acceptance_resting_speed_negative_control
        PRIVATE CK3_RESTING_SPEED_PREFIX_CONTROL=1)
    target_link_libraries(acceptance_resting_speed_negative_control
        PRIVATE Threads::Threads)
    target_compile_options(acceptance_resting_speed_negative_control
        PRIVATE -O2 -Wall -Wextra)
else()
    message(STATUS "fixture dir absent; skipping acceptance_resting_speed_negative_control")
endif()

    # ck-sd4d: the ended-war ledger and the war-IDENTITY rule. war_history.cpp
    # is engine-free by construction (no offsets header, no globals), so the
    # rule that keeps the two 1066 k_england wars apart is testable with no
    # CK3 process at all.
    add_executable(test_war_history
        test/test_war_history.cpp
        src/war_history.cpp
    )
    target_include_directories(test_war_history PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(test_war_history PRIVATE Threads::Threads)
    target_compile_options(test_war_history PRIVATE -O2 -Wall -Wextra)

    # ck-hxh7: world-reload detection + world-scoped cache invalidation.
    # Compiles world_epoch.cpp and name_tables.cpp standalone; the engine name
    # resolver is replaced through set_character_name_resolver().
    add_executable(test_world_epoch
        test/test_world_epoch.cpp
        src/world_epoch.cpp
        src/name_tables.cpp
    )
    target_include_directories(test_world_epoch PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_link_libraries(test_world_epoch PRIVATE Threads::Threads)
    target_compile_options(test_world_epoch PRIVATE -O2 -Wall -Wextra)

    # ck-hxh7 ACCEPTANCE: a save reload drops every world-scoped cache. Links the
    # REAL world_epoch.cpp / name_tables.cpp / war_history.cpp /
    # personality_world_reset.cpp. Driven by scripts/acceptance_world_reset.py,
    # which adds the structural lane for the three caches that live inside
    # engine-coupled TUs and cannot be linked on a host.
    add_executable(acceptance_world_reset
        test/acceptance_world_reset.cpp
        src/world_epoch.cpp
        src/name_tables.cpp
        src/war_history.cpp
        src/personality_world_reset.cpp
    )
    target_include_directories(acceptance_world_reset PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_link_libraries(acceptance_world_reset PRIVATE Threads::Threads)
    target_compile_options(acceptance_world_reset PRIVATE -O2 -Wall -Wextra)

    # ck-9fc3 ACCEPTANCE: quarantined agent steering is restored ONLY when the
    # loaded world is PROVABLY the same save. Links the REAL world_identity.cpp,
    # personality_world_reset.cpp, personality_persist.cpp and world_epoch.cpp —
    # every one engine-free — so the negative control ("a DIFFERENT save must not
    # restore") runs with no CK3 binary. Driven by
    # scripts/acceptance_save_identity.py, which adds the structural lane.
    add_executable(acceptance_save_identity
        test/acceptance_save_identity.cpp
        src/world_epoch.cpp
        src/world_identity.cpp
        src/board.cpp
        src/board_persist.cpp
        src/episode_policy.cpp
        src/action_args.cpp
        src/personality_world_reset.cpp
        src/personality_persist.cpp
    )
    target_include_directories(acceptance_save_identity PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_link_libraries(acceptance_save_identity PRIVATE Threads::Threads)
    target_compile_options(acceptance_save_identity PRIVATE -O2 -Wall -Wextra)

    # ck-22n3: the .init_array CONTRACT, asserted against the BUILT .so —
    # every C++ dynamic initializer runs BEFORE dll_init(). dll_init starts
    # threads (reader/server/conductor/game-ready poller) and writes globals
    # synchronously; without init_priority(101) all of that ran against raw
    # .bss. Reads the .so as a FILE — it must never dlopen it, which would run
    # dll_init here. Depends on the library so it always tests a fresh build.
    add_executable(test_init_order
        test/test_init_order.cpp
    )
    target_include_directories(test_init_order PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_definitions(test_init_order PRIVATE
        CK3_BRIDGE_SO_PATH="$<TARGET_FILE:ck3_bridge>")
    target_compile_options(test_init_order PRIVATE -O2 -Wall -Wextra)
    add_dependencies(test_init_order ck3_bridge)
    # ck-6kvq.12 (R4): the shared append-only board. Pure by construction — no
    # engine, no game, no calendar — because "who wrote this post, in which
    # flush, on which game date" must be provable without a running game, and
    # because the arm gate is the paper's manipulation and has to be testable on
    # any host. episode_policy.cpp rides along: the ARM is episode config.
    add_executable(test_board
        test/test_board.cpp
        src/board.cpp
        src/board_persist.cpp
        src/episode_policy.cpp
        src/action_args.cpp
    )
    target_include_directories(test_board PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
        # ck-2ngg: episode_policy.cpp resolves X through exclusion_rule.h, which
        # spans the horizon on the ENGINE's calendar (ck3::game_date). Constants
        # only — still no engine reads, still no CK3 binary.
        ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets
    )
    target_link_libraries(test_board PRIVATE Threads::Threads)
    target_compile_options(test_board PRIVATE -O2 -Wall -Wextra)

    # ck-6kvq.12 ACCEPTANCE: posts are attributed and ordered, a control session
    # cannot forge an author, quota refusals are typed, and a new episode starts
    # at seq 0. Links the REAL board.cpp AND the REAL session.cpp, so the
    # attribution it checks is minted from the same ownership table the dispatch
    # choke point uses. Driven by scripts/acceptance_board.py, which adds the
    # arm-gate lane and the negative control.
    add_executable(acceptance_board
        test/acceptance_board.cpp
        ${SESSION_HOST_SOURCES}
    )
    target_include_directories(acceptance_board PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_link_libraries(acceptance_board PRIVATE Threads::Threads)
    target_compile_options(acceptance_board PRIVATE -O2 -Wall -Wextra)

    # ck-5gdx: THE BOUNDS WITNESS PROBE. Prints what board_init_from_env()
    # resolved AND what stats_json() publishes, over whatever environment the
    # caller set — so scripts/acceptance_board_witness.py gets its fixtures from
    # the BRIDGE'S OWN EMISSION rather than from a JSON blob someone typed. The
    # acceptance also compiles this against the PRE-FIX board.cpp/board.h as its
    # negative control, which is why the probe is a separate, minimal TU.
    add_executable(board_witness_probe
        test/board_witness_probe.cpp
        ${SESSION_HOST_SOURCES}
    )
    target_include_directories(board_witness_probe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_link_libraries(board_witness_probe PRIVATE Threads::Threads)
    target_compile_options(board_witness_probe PRIVATE -O2 -Wall -Wextra)

    # ck-6kvq.5 ACCEPTANCE (R7c/R2a): a session survives a socket drop, resumes
    # with its claims INTACT, gets its steering back on the same save and NOT on
    # a different one, and the unavailability span for the gap is RECORDED, not
    # reset. Links the REAL session.cpp, the REAL world_identity.cpp and the REAL
    # unavailability.cpp/episode_policy.cpp — so the whole chain that decides an
    # episode's validity is exercised with no CK3 binary. Driven by
    # scripts/acceptance_session_resume.py.
    set(RESUME_ACCEPTANCE_SOURCES
        test/acceptance_session_resume.cpp
        ${SESSION_HOST_SOURCES}
        src/unavailability.cpp
    )
    add_executable(acceptance_session_resume ${RESUME_ACCEPTANCE_SOURCES})
    target_include_directories(acceptance_session_resume PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_link_libraries(acceptance_session_resume PRIVATE Threads::Threads)
    target_compile_options(acceptance_session_resume PRIVATE -O2 -Wall -Wextra)

    # THE NEGATIVE CONTROL. Identical source, one macro: the resume identity
    # clause always allows. The ck-9fc3 symptom comes straight back — a session
    # keeps claims (and therefore steering) over characters in a world it never
    # saw — and this binary MUST FAIL. An acceptance that cannot fail is not
    # evidence.
    add_executable(acceptance_session_resume_negative_control ${RESUME_ACCEPTANCE_SOURCES})
    target_include_directories(acceptance_session_resume_negative_control PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_link_libraries(acceptance_session_resume_negative_control PRIVATE Threads::Threads)
    target_compile_options(acceptance_session_resume_negative_control PRIVATE
        -O2 -Wall -Wextra -DCK3_RESUME_NO_IDENTITY_CHECK)

    # ck-bw1s THE SECOND NEGATIVE CONTROL. Identical source, one macro: the
    # detach report counts EVERY non-attached session — what it did before this
    # bead — so a dropped OBSERVE session publishes state/detached-since and the
    # abort ladder aborts a healthy arm episode reason=session_lost because an
    # observer blinked. This binary MUST FAIL, and must fail on the OBSERVER
    # lane specifically: a control that fails to COMPILE is a control doing
    # nothing (ck-81su).
    add_executable(acceptance_session_resume_observer_negative_control
        ${RESUME_ACCEPTANCE_SOURCES})
    target_include_directories(acceptance_session_resume_observer_negative_control PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_link_libraries(acceptance_session_resume_observer_negative_control
        PRIVATE Threads::Threads)
    target_compile_options(acceptance_session_resume_observer_negative_control PRIVATE
        -O2 -Wall -Wextra -DCK3_DETACH_REPORT_COUNTS_OBSERVERS)

    # ck-81su ACCEPTANCE: the board survives a PROCESS RELAUNCH with its seq
    # continuing, and only under a DECLARED and ACCEPTED save identity. Each
    # phase is a fresh process driven by scripts/acceptance_board_continuity.py,
    # because `begin_episode()` resetting seq on process start IS the bug and a
    # single-process test would only be testing a simulation of it. Links the
    # REAL board.cpp, board_persist.cpp and world_identity.cpp — one judge, one
    # decision, no CK3.
    set(BOARD_CONTINUITY_SOURCES
        test/acceptance_board_continuity.cpp
        src/board.cpp
        src/board_persist.cpp
        src/episode_policy.cpp
        src/action_args.cpp
        src/world_identity.cpp
        src/world_epoch.cpp
        src/personality_world_reset.cpp
        src/personality_persist.cpp
    )
    add_executable(acceptance_board_continuity ${BOARD_CONTINUITY_SOURCES})
    target_include_directories(acceptance_board_continuity PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_link_libraries(acceptance_board_continuity PRIVATE Threads::Threads)
    target_compile_options(acceptance_board_continuity PRIVATE -O2 -Wall -Wextra)

    # THE NEGATIVE CONTROL. Identical source, one macro: the carrier is found and
    # IGNORED — the pre-fix bridge. Its `resume` phase MUST FAIL, and must fail
    # on the seq restart. An acceptance that cannot fail is not evidence.
    add_executable(acceptance_board_continuity_negative_control
        ${BOARD_CONTINUITY_SOURCES})
    target_include_directories(acceptance_board_continuity_negative_control PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_compile_definitions(acceptance_board_continuity_negative_control PRIVATE
        CK3_BOARD_NO_CARRIER_RESTORE=1)
    target_link_libraries(acceptance_board_continuity_negative_control PRIVATE
        Threads::Threads)
    target_compile_options(acceptance_board_continuity_negative_control PRIVATE
        -O2 -Wall -Wextra)

    # ck-hxh7 grep gate: every world-scoped cache clear function must have a real
    # caller. The original bug was clear_name_cache() existing with ZERO callers.
    # Runs on every build of the test targets; a regression fails the build.
    add_custom_target(bridge_cache_invalidation_gate ALL
        COMMAND ${CMAKE_COMMAND} -E env bash
                ${CMAKE_CURRENT_SOURCE_DIR}/test/gate_cache_invalidation.sh
                ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "ck-hxh7 gate: world-scoped cache clears have callers"
        VERBATIM
    )

    # Engine-free tests that run standalone and exit 0; `ctest` runs them.
    enable_testing()
    foreach(_t test_json_helpers test_script_block_parser test_instance_paths
               test_game_date test_war_history test_clock_rate
               test_flush_scheduler test_pipeline
               test_debug_verbs test_ipc_wiring test_handoff_player)
        add_test(NAME ${_t} COMMAND ${_t})
    endforeach()
    # rc2 B2: the ON build must fail the same assertions.
    add_test(NAME test_debug_verbs_negative_control
             COMMAND test_debug_verbs_negative_control)
    set_tests_properties(test_debug_verbs_negative_control PROPERTIES WILL_FAIL TRUE)
    # rc2 B1/B2 source censuses (python, no build products). Each has a
    # selftest (known-bad text MUST be flagged) and the B2 ones a control that
    # reads the tree as a debug build and MUST fail.
    # Each script is spelled out: release/export.py's bridge/test census
    # matches file names literally.
    set(_census_private_file_opens ${CMAKE_CURRENT_SOURCE_DIR}/test/check_private_file_opens.py)
    set(_census_raw_pointer_inputs ${CMAKE_CURRENT_SOURCE_DIR}/test/check_raw_pointer_inputs.py)
    set(_census_no_va_output       ${CMAKE_CURRENT_SOURCE_DIR}/test/check_no_va_output.py)
    foreach(_c private_file_opens raw_pointer_inputs no_va_output)
        add_test(NAME ${_c}
                 COMMAND ${CK3_PYTHON3} ${_census_${_c}} ${CMAKE_CURRENT_SOURCE_DIR})
        add_test(NAME ${_c}_selftest
                 COMMAND ${CK3_PYTHON3} ${_census_${_c}} --selftest)
    endforeach()
    foreach(_c raw_pointer_inputs no_va_output)
        add_test(NAME ${_c}_debug_control
                 COMMAND ${CK3_PYTHON3} ${_census_${_c}} --include-debug
                         ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(${_c}_debug_control PROPERTIES WILL_FAIL TRUE)
    endforeach()

    # rc2 B1 (ck-otj9): log rotation reopens O_EXCL|O_NOFOLLOW at 0600. Was
    # run only by host acceptance_otj9.py. Exit 3 = cannot set up (skip).
    # EXISTS: release/manifest.toml still excludes the probe from the public
    # tree; drop that exclusion and the public ctest runs it too.
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/log_rotate_probe.cpp)
        add_executable(log_rotate_probe test/log_rotate_probe.cpp)
        target_include_directories(log_rotate_probe PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../re/offsets
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
        add_test(NAME log_rotate_probe COMMAND log_rotate_probe)
        set_tests_properties(log_rotate_probe PROPERTIES SKIP_RETURN_CODE 3)
    endif()

    # rc2 B2: the shipped .so carries none of the debug handlers.
    if(NOT CK3_BRIDGE_DEBUG_VERBS)
        add_test(NAME debug_verbs_not_linked
                 COMMAND ${CK3_PYTHON3}
                         ${CMAKE_CURRENT_SOURCE_DIR}/test/check_debug_verbs_not_linked.py
                         $<TARGET_FILE:ck3_bridge>)
    endif()
endif()

# -----------------------------------------------------------------------------
# ck3mapgen: standalone build-time map-processor (bakes ck3map.bin).
# OPT-IN so it never affects the default libck3_bridge.so build. Enable with:
#   cmake -DCK3_BUILD_MAPGEN=ON ..
# It is fully self-contained (its own CMakeLists, only stb_image.h + stdlib);
# building/running it is normally driven by `scripts/bdt.sh mapgen` instead.
# -----------------------------------------------------------------------------
option(CK3_BUILD_MAPGEN "Build the standalone ck3mapgen map-processor" OFF)
if(CK3_BUILD_MAPGEN)
    add_subdirectory(tools/ck3mapgen)
endif()
