# GH2 Recomp's game layer, linked into the runtime's runner as objects
# (PS2X_GAME_DIR). Reuses GH Classic's synth module from ../src/synth.

set(GHC_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src")

add_library(ps2x_game OBJECT
    install.cpp
    pad.cpp
    synth.cpp
    audio.cpp
    trace.cpp
    vu1.cpp
    venue.cpp
    flares.cpp
    movie_audio.cpp
    launch.cpp
    ${GHC_SRC}/synth/module.cpp
    ${GHC_SRC}/synth/spu.cpp
    ${GHC_SRC}/synth/reverb.cpp
)
target_include_directories(ps2x_game PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${GHC_SRC}")
target_link_libraries(ps2x_game PRIVATE ps2_runtime)
target_compile_features(ps2x_game PRIVATE cxx_std_20)
# The product's name: GH2Recomp.ini, gh2_programs.bin, the window's title.
target_compile_definitions(ps2x_game PRIVATE GHR_NAME="GH2" GHR_SLUG="gh2")

# Video and input: PCSX2's GS (GPL-3.0-or-later) drawing into an SDL3 window,
# when both checkouts are given (tools/gh1.py pins and fetches them). Without
# them the runtime's own software GS and raylib window are used.
set(GH1_PCSX2_DIR "" CACHE PATH "PCSX2 checkout, for its GS")
set(GH1_SDL3_DIR "" CACHE PATH "SDL3 checkout, for the window, input and audio")
if(GH1_PCSX2_DIR AND GH1_SDL3_DIR)
    set(PCSX2_DIR "${GH1_PCSX2_DIR}" CACHE PATH "PCSX2 source checkout" FORCE)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../pcsx2gs" "${CMAKE_BINARY_DIR}/pcsx2gs" EXCLUDE_FROM_ALL)

    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
    set(SDL_GPU OFF CACHE BOOL "" FORCE)
    set(SDL_RENDER OFF CACHE BOOL "" FORCE)
    set(SDL_VULKAN OFF CACHE BOOL "" FORCE)
    set(SDL_OPENGLES OFF CACHE BOOL "" FORCE)
    set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
    set(SDL_X11_XSCRNSAVER OFF CACHE BOOL "" FORCE)
    set(SDL_X11_XTEST OFF CACHE BOOL "" FORCE)
    add_subdirectory("${GH1_SDL3_DIR}" "${CMAKE_BINARY_DIR}/sdl3" EXCLUDE_FROM_ALL)

    target_sources(ps2x_game PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../pcsx2gs/host/pcsx2gs_sdl_host.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../pcsx2gs/host/guitars.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../pcsx2gs/host/keyboard.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../pcsx2gs/host/keyboard_options.cpp")
    target_include_directories(ps2x_game PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../pcsx2gs/host")
    target_compile_definitions(ps2x_game PRIVATE GH1_PCSX2GS=1 GH1_PCSX2VU=1)
    # VU1: PCSX2's microVU recompiler (see vu1.cpp).
    target_link_libraries(ps2x_game PRIVATE pcsx2gs_bridge pcsx2vu_bridge SDL3::SDL3-static)
    if(WIN32)
        target_link_libraries(ps2x_game PRIVATE winmm) # timeBeginPeriod
    endif()
    # PCSX2's glad and the one inside raylib define the same symbols.
    set(PS2X_LOCALIZE_RAYLIB_GLAD ON PARENT_SCOPE)
endif()
