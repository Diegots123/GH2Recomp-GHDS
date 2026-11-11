# PCSX2's GS (GPL-3.0-or-later), built on its own as the PS2 graphics backend
# for GH1 Recomp. Only the GS, PCSX2's common library and the third-party code
# they need are compiled; shim/ stands in for the rest of the emulator (host,
# VM manager, configuration, overlays). Texture replacement and video capture
# are left out: no mods, nothing GH1 needs.
#
#   PCSX2_DIR  a PCSX2 checkout (pinned in tools/gh1.py)

cmake_minimum_required(VERSION 3.20)
project(pcsx2gs C CXX)
find_package(Threads REQUIRED)
set(PCSX2_DIR "" CACHE PATH "PCSX2 source checkout")
if(NOT PCSX2_DIR)
    message(FATAL_ERROR "set PCSX2_DIR")
endif()
set(P ${PCSX2_DIR})

# --- third party -----------------------------------------------------------
add_subdirectory(${P}/3rdparty/fmt ${CMAKE_BINARY_DIR}/pcsx2-fmt EXCLUDE_FROM_ALL)
add_subdirectory(${P}/3rdparty/fast_float ${CMAKE_BINARY_DIR}/pcsx2-fast_float EXCLUDE_FROM_ALL)
add_subdirectory(${P}/3rdparty/glad ${CMAKE_BINARY_DIR}/pcsx2-glad EXCLUDE_FROM_ALL)
# imgui without FreeType/plutosvg: PCSX2's overlays are not used.
set(IM ${P}/3rdparty/imgui)
add_library(imgui STATIC ${IM}/src/imgui.cpp ${IM}/src/imgui_draw.cpp ${IM}/src/imgui_tables.cpp
    ${IM}/src/imgui_widgets.cpp ${IM}/src/imgui_stdlib.cpp)
target_include_directories(imgui PUBLIC ${IM}/include PRIVATE ${IM}/src)
# PCSX2's imconfig.h asks for FreeType; its built-in stb_truetype loader is
# compiled in instead and handed back by shim's ImGuiFreeType::GetFontLoader.
target_compile_definitions(imgui PUBLIC IMGUI_ENABLE_STB_TRUETYPE)
add_subdirectory(${P}/3rdparty/cpuinfo ${CMAKE_BINARY_DIR}/pcsx2-cpuinfo EXCLUDE_FROM_ALL)
add_subdirectory(${P}/3rdparty/lzma ${CMAKE_BINARY_DIR}/pcsx2-lzma EXCLUDE_FROM_ALL)

add_library(pcsx2gs_flags INTERFACE)
target_compile_features(pcsx2gs_flags INTERFACE cxx_std_20)
target_include_directories(pcsx2gs_flags INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/shim/include  # before PCSX2's own, to override
    ${P} ${P}/pcsx2 ${P}/common/include ${P}/3rdparty/include ${P}/3rdparty/xbyak
    ${P}/3rdparty/simpleini/include ${P}/3rdparty/vulkan/include)
target_compile_definitions(pcsx2gs_flags INTERFACE
    _ARCH_64=1 _M_X86=1 __M_X86_64=1 PCSX2_GS_STANDALONE=1 ENABLE_OPENGL=1)
target_compile_options(pcsx2gs_flags INTERFACE -msse4.1 -Wno-deprecated-declarations -Wno-unused-parameter)
if(MSVC)
    # clang-cl, as PCSX2's own Windows build sets it up (its aliasing is
    # already relaxed).
    target_compile_definitions(pcsx2gs_flags INTERFACE __SSE4_1__=1 _CRT_NONSTDC_NO_WARNINGS
        _CRT_SECURE_NO_WARNINGS _SCL_SECURE_NO_WARNINGS UNICODE _UNICODE)
    target_include_directories(pcsx2gs_flags INTERFACE ${P}/3rdparty/winwil/include)
else()
    target_compile_options(pcsx2gs_flags INTERFACE -fno-strict-aliasing)
endif()
target_link_libraries(pcsx2gs_flags INTERFACE fmt::fmt fast_float)

# --- common ------------------------------------------------------------------
set(C ${P}/common)
add_library(pcsx2_common STATIC
    ${C}/AlignedMalloc.cpp ${C}/Assertions.cpp ${C}/Console.cpp ${C}/DynamicLibrary.cpp ${C}/Error.cpp
    ${C}/FastJmp.cpp ${C}/FileSystem.cpp ${C}/HostSys.cpp ${C}/MD5Digest.cpp ${C}/Perf.cpp
    ${C}/ProgressCallback.cpp ${C}/ReadbackSpinManager.cpp ${C}/Semaphore.cpp ${C}/SmallString.cpp
    ${C}/StringUtil.cpp ${C}/Timer.cpp ${C}/WindowInfo.cpp ${C}/SettingsWrapper.cpp
    ${C}/MemorySettingsInterface.cpp)
if(WIN32)
    target_sources(pcsx2_common PRIVATE ${C}/Windows/WinThreads.cpp ${C}/Windows/WinHostSys.cpp ${C}/Windows/WinMisc.cpp)
else()
    target_sources(pcsx2_common PRIVATE ${C}/Linux/LnxHostSys.cpp ${C}/Linux/LnxThreads.cpp ${C}/Linux/LnxMisc.cpp)
endif()
target_link_libraries(pcsx2_common PUBLIC pcsx2gs_flags cpuinfo)
if(NOT WIN32)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DBUS REQUIRED dbus-1)
    target_include_directories(pcsx2_common PRIVATE ${DBUS_INCLUDE_DIRS})
    target_link_libraries(pcsx2_common PRIVATE ${DBUS_LINK_LIBRARIES})
endif()

# --- GS ----------------------------------------------------------------------
set(G ${P}/pcsx2/GS)
add_library(pcsx2gs STATIC
    ${G}/GS.cpp ${G}/GSBlock.cpp ${G}/GSXXH.cpp ${G}/GSClut.cpp ${G}/GSDrawingContext.cpp ${G}/GSDrawingEnvironment.cpp
    ${G}/GSLocalMemory.cpp ${G}/GSLocalMemoryMultiISA.cpp ${G}/GSPerfMon.cpp ${G}/GSRingHeap.cpp
    ${G}/GSState.cpp ${G}/GSTables.cpp ${G}/GSUtil.cpp ${G}/GSVector.cpp ${G}/MultiISA.cpp
    ${G}/Renderers/Common/GSDevice.cpp ${G}/Renderers/Common/GSDirtyRect.cpp
    ${G}/Renderers/Common/GSFunctionMap.cpp ${G}/Renderers/Common/GSRenderer.cpp
    ${G}/Renderers/Common/GSTexture.cpp ${G}/Renderers/Common/GSVertexTrace.cpp
    ${G}/Renderers/Common/GSVertexTraceFMM.cpp
    ${G}/Renderers/Null/GSRendererNull.cpp
    ${G}/Renderers/HW/GSHwHack.cpp ${G}/Renderers/HW/GSRendererHW.cpp ${G}/Renderers/HW/GSRendererHWMultiISA.cpp
    ${G}/Renderers/HW/GSTextureCache.cpp
    ${G}/Renderers/SW/GSDrawScanline.cpp ${G}/Renderers/SW/GSRasterizer.cpp ${G}/Renderers/SW/GSRendererSW.cpp
    ${G}/Renderers/SW/GSTextureCacheSW.cpp
    ${G}/Renderers/SW/GSDrawScanlineCodeGenerator.all.cpp ${G}/Renderers/SW/GSSetupPrimCodeGenerator.all.cpp
    ${G}/Renderers/OpenGL/GLContext.cpp ${G}/Renderers/OpenGL/GLProgram.cpp ${G}/Renderers/OpenGL/GLShaderCache.cpp
    ${G}/Renderers/OpenGL/GLState.cpp ${G}/Renderers/OpenGL/GLStreamBuffer.cpp
    ${G}/Renderers/OpenGL/GSDeviceOGL.cpp ${G}/Renderers/OpenGL/GSTextureOGL.cpp)
if(WIN32)
    # OpenGL and Direct3D 11 (the renderer PCSX2 picks for older GPUs, and
    # often the faster one on integrated graphics). shim/include/GS/Renderers/DX12
    # stands in for the Direct3D 12 device GS.cpp names. Direct3D 11 compiles
    # its shaders with d3dcompiler_47.dll, part of Windows; D3D.cpp's DXC path
    # (Direct3D 12 only) is delay-loaded, so dxcompiler.dll is never needed.
    target_sources(pcsx2gs PRIVATE ${G}/Renderers/OpenGL/GLContextWGL.cpp
        ${G}/Renderers/DX11/D3D.cpp ${G}/Renderers/DX11/D3D11ShaderCache.cpp
        ${G}/Renderers/DX11/GSDevice11.cpp ${G}/Renderers/DX11/GSTexture11.cpp)
    target_link_libraries(pcsx2gs PUBLIC opengl32 dwmapi shlwapi onecore d3d11 dxgi d3dcompiler dxguid
        dxcompiler delayimp)
    target_link_options(pcsx2gs PUBLIC /DELAYLOAD:dxcompiler.dll)
else()
    target_sources(pcsx2gs PRIVATE ${G}/Renderers/OpenGL/GLContextEGL.cpp ${G}/Renderers/OpenGL/GLContextEGLX11.cpp)
endif()
# PCSX2's own configuration code, unmodified, for GSOptions' real defaults.
target_sources(pcsx2gs PRIVATE ${P}/pcsx2/Pcsx2Config.cpp ${P}/pcsx2/SourceLog.cpp ${CMAKE_CURRENT_SOURCE_DIR}/shim/stubs.cpp)
target_link_libraries(pcsx2gs PUBLIC pcsx2_common glad imgui cpuinfo)
if(NOT WIN32)
    find_package(X11 REQUIRED)
    target_link_libraries(pcsx2gs PUBLIC X11::X11 X11::Xi)
    target_compile_definitions(pcsx2gs_flags INTERFACE X11_API=1)
endif()

# Shaders baked into the binary, as PCSX2 does with BAKE_SHADERS_IN_CPP.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(SHADER_CPP_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders_cpp)
file(MAKE_DIRECTORY ${SHADER_CPP_DIR})
# GSDevice.cpp includes every API's baked shaders (DX11's on Windows only).
set(SH ${P}/bin/resources/shaders)
file(GLOB SHADER_SOURCES ${SH}/common/*.glsl ${SH}/common/*.fx ${SH}/common/*.h
    ${SH}/opengl/*.glsl ${SH}/vulkan/*.glsl)
if(WIN32)
    file(GLOB SHADER_SOURCES_DX11 ${SH}/dx11/*.fx ${SH}/dx11/*.hlsl)
    list(APPEND SHADER_SOURCES ${SHADER_SOURCES_DX11})
endif()
set(SHADER_CPPS "")
foreach(path IN LISTS SHADER_SOURCES)
    get_filename_component(DIR ${path} DIRECTORY)
    get_filename_component(API ${DIR} NAME)
    get_filename_component(BASE ${path} NAME_WE)
    set(cpp_path ${SHADER_CPP_DIR}/${API}_${BASE}.cpp)
    add_custom_command(OUTPUT ${cpp_path}
        COMMAND ${Python3_EXECUTABLE} ${P}/tools/shader_to_cpp.py ${path} ${cpp_path} "${API}_${BASE}"
        DEPENDS ${path} ${P}/tools/shader_to_cpp.py VERBATIM)
    list(APPEND SHADER_CPPS ${cpp_path})
endforeach()
add_custom_target(pcsx2gs_shaders DEPENDS ${SHADER_CPPS})
add_dependencies(pcsx2gs pcsx2gs_shaders)
target_compile_definitions(pcsx2gs PUBLIC BAKE_SHADERS_IN_CPP=1)
target_include_directories(pcsx2gs PUBLIC ${SHADER_CPP_DIR})

# --- VU1: PCSX2's microVU recompiler, built on its own like the GS ----------
# vushim/ stands in for the rest of the emulator around it; its Gif_Unit.h
# replaces PCSX2's, so XGKICK's packets go to the runtime's GIF arbiter.
add_library(pcsx2vu STATIC
    ${P}/pcsx2/x86/microVU.cpp
    ${C}/emitter/bmi.cpp ${C}/emitter/fpu.cpp ${C}/emitter/groups.cpp ${C}/emitter/jmp.cpp
    ${C}/emitter/legacy.cpp ${C}/emitter/legacy_sse.cpp ${C}/emitter/movs.cpp ${C}/emitter/simd.cpp
    ${C}/emitter/x86emitter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vushim/vushim.cpp)
target_include_directories(pcsx2vu BEFORE PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/vushim/include)
target_include_directories(pcsx2vu PRIVATE ${P}/pcsx2/x86)
# EmuConfig, g_cpu and the executable memory come with the GS library.
target_link_libraries(pcsx2vu PUBLIC pcsx2gs pcsx2_common)

add_library(pcsx2vu_bridge STATIC ${CMAKE_CURRENT_SOURCE_DIR}/bridge/pcsx2vu_bridge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/bridge/stall_probe.cpp)
# Where a stalled thread was (Windows: the thread suspended, its context and
# stack read). Off in releases: antivirus heuristics flag unsigned exes that
# read other threads' contexts.
option(GHR_STALL_SAMPLES "stall_probe: sample where a stalled thread is" OFF)
if(GHR_STALL_SAMPLES)
    target_compile_definitions(pcsx2vu_bridge PRIVATE GHR_STALL_SAMPLES=1)
endif()
target_link_libraries(pcsx2vu_bridge PRIVATE pcsx2vu)
target_include_directories(pcsx2vu_bridge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/bridge)

# --- the bridge: the GS behind a plain interface (bridge/pcsx2gs_bridge.h), so
# the host side never sees PCSX2's headers or compile flags -------------------
add_library(pcsx2gs_bridge STATIC ${CMAKE_CURRENT_SOURCE_DIR}/bridge/pcsx2gs_bridge.cpp)
target_link_libraries(pcsx2gs_bridge PRIVATE pcsx2gs)
target_include_directories(pcsx2gs_bridge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/bridge)

# --- smoke test: open the GS without a window, to find what the shim needs ----
if(NOT WIN32)
    add_executable(pcsx2gs_smoke ${CMAKE_CURRENT_SOURCE_DIR}/shim/smoke.cpp)
    target_link_libraries(pcsx2gs_smoke PRIVATE pcsx2gs)
endif()

# --- smoke test: a tiny VU1 program through the microVU bridge ----------------
add_executable(pcsx2vu_smoke ${CMAKE_CURRENT_SOURCE_DIR}/vushim/vu_smoke.cpp)
target_link_libraries(pcsx2vu_smoke PRIVATE pcsx2vu_bridge)
add_executable(pcsx2vu_replay ${CMAKE_CURRENT_SOURCE_DIR}/vushim/vu_replay.cpp)
target_link_libraries(pcsx2vu_replay PRIVATE pcsx2vu_bridge)
