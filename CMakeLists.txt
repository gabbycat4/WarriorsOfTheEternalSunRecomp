cmake_minimum_required(VERSION 3.19)
project(WarriorsOfTheEternalSunRecomp VERSION 0.0.1 LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# ---------------------------------------------------------------------------
# Dungeons & Dragons: Warriors of the Eternal Sun (Westwood / Sega, 1992),
# USA/Europe. Native static recompilation on segagenesisrecomp.
# Layout copied from RocketKnightAdventuresRecomp (no community disassembly,
# flat 1 MB cartridge), but using the engine's runner module
# (cmake/GenesisRecompRunner.cmake) instead of a hand-written source list.
# Game-owned configuration and adapter live in game/.
# ---------------------------------------------------------------------------

set(GENESIS_RECOMP_ROOT "" CACHE PATH
    "Optional shared Genesis framework checkout (defaults to the submodule)")
if(GENESIS_RECOMP_ROOT)
    set(RECOMP_ROOT "${GENESIS_RECOMP_ROOT}")
    message(STATUS "engine: explicit GENESIS_RECOMP_ROOT override")
else()
    set(RECOMP_ROOT "${CMAKE_SOURCE_DIR}/segagenesisrecomp")
    message(STATUS "engine: committed segagenesisrecomp/ submodule")
endif()
set(RUNNER_ROOT "${RECOMP_ROOT}/runner")

find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)

option(GEN_ENABLE_TRACE
    "Build the TCP cmd server + observability rings. OFF for release." ON)
if(GEN_ENABLE_TRACE)
    add_compile_definitions(GEN_ENABLE_TRACE=1)
endif()
option(SONIC_REVERSE_DEBUG
    "Tier-1 reverse-debug instrumentation (WRAM store ring; dev builds)" ON)

# Generated C (from YOUR ROM at game/wotes.bin) + the runner source list.
include("${RECOMP_ROOT}/cmake/GenesisRecompGenerated.cmake")
genesisrecomp_add_generated_sources(
    GENERATED_SOURCES wotes "${CMAKE_CURRENT_SOURCE_DIR}/game" wotes.bin
    REVERSE_DEBUG "${SONIC_REVERSE_DEBUG}")
genesisrecomp_runner_sources(RUNNER_SOURCES
    TRACE ${GEN_ENABLE_TRACE} REVERSE_DEBUG ${SONIC_REVERSE_DEBUG})

add_executable(WotESRecomp
    ${RUNNER_SOURCES}
    ${GENERATED_SOURCES}
    game/wotes_spec.c
    game/wotes_motion.c
    game/wotes_video.c)
add_dependencies(WotESRecomp ${GENERATED_SOURCES_TARGET})
target_compile_definitions(WotESRecomp PRIVATE
    INCLUDE_GENERATED_CODE=1 ENABLE_RECOMPILED_CODE=1 OWN_BACKEND=1
    DEFAULT_DEBUG_PORT=4400
    $<$<BOOL:${SONIC_REVERSE_DEBUG}>:SONIC_REVERSE_DEBUG=1>)
target_include_directories(WotESRecomp PRIVATE
    "${RUNNER_ROOT}" "${RUNNER_ROOT}/include" "${RUNNER_ROOT}/video"
    "${RUNNER_ROOT}/external/ymfm/src"
    "${RUNNER_ROOT}/external/superzazu"
    "${RUNNER_ROOT}/external/clowncommon"
    "${RECOMP_ROOT}/recompiler/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/game")
target_link_libraries(WotESRecomp PRIVATE SDL2::SDL2main SDL2::SDL2 OpenGL::GL)
genesisrecomp_runner_target(WotESRecomp)
genesisrecomp_native_optimization(WotESRecomp)

# Shared recomp-ui launcher (same as RKA and your NES ports).
include(${CMAKE_SOURCE_DIR}/recomp-ui/recomp_ui.cmake)
recomp_target_launcher_ui(WotESRecomp CONSOLE genesis)
