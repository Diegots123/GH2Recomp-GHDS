# Cross-compiling the Windows build from Linux: clang-cl and lld-link against
# Microsoft's C runtime and Windows SDK, laid out by xwin
# (`xwin --accept-license --arch x86_64 splat --use-winsysroot-style --output <dir>`).
#
#   cmake -DCMAKE_TOOLCHAIN_FILE=tools/cmake/windows-clang-cl.cmake -DXWIN_DIR=<dir> ...

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(NOT XWIN_DIR)
    set(XWIN_DIR "$ENV{XWIN_DIR}")
endif()
if(NOT XWIN_DIR)
    message(FATAL_ERROR "set XWIN_DIR to an xwin splat made with --use-winsysroot-style")
endif()
set(XWIN_DIR "${XWIN_DIR}" CACHE PATH "xwin splat (winsysroot style)")
# try_compile projects see the toolchain file again, without the cache.
set(ENV{XWIN_DIR} "${XWIN_DIR}")

# clang-cl's driver mode comes from its name. Microsoft's STL in the current
# Visual Studio wants Clang 19 or newer.
find_program(GH1_CLANG_CL NAMES clang-cl-20 clang-cl REQUIRED)
find_program(GH1_LLD_LINK NAMES lld-link-20 lld-link REQUIRED)
find_program(GH1_LLVM_LIB NAMES llvm-lib-20 llvm-lib REQUIRED)
find_program(GH1_LLVM_RC NAMES llvm-rc-20 llvm-rc REQUIRED)
find_program(GH1_LLVM_MT NAMES llvm-mt-20 llvm-mt REQUIRED)

set(CMAKE_C_COMPILER "${GH1_CLANG_CL}")
set(CMAKE_CXX_COMPILER "${GH1_CLANG_CL}")
set(CMAKE_LINKER "${GH1_LLD_LINK}")
set(CMAKE_AR "${GH1_LLVM_LIB}")
set(CMAKE_RC_COMPILER "${GH1_LLVM_RC}")
set(CMAKE_MT "${GH1_LLVM_MT}")
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)

file(GLOB _gh1_msvc LIST_DIRECTORIES true "${XWIN_DIR}/VC/Tools/MSVC/*")
file(GLOB _gh1_sdk_inc LIST_DIRECTORIES true "${XWIN_DIR}/Windows Kits/10/include/*")
file(GLOB _gh1_sdk_lib LIST_DIRECTORIES true "${XWIN_DIR}/Windows Kits/10/lib/*")
list(GET _gh1_msvc 0 _gh1_msvc)
list(GET _gh1_sdk_inc 0 _gh1_sdk_inc)
list(GET _gh1_sdk_lib 0 _gh1_sdk_lib)

set(_gh1_includes "")
foreach(dir "${_gh1_msvc}/include" "${_gh1_sdk_inc}/ucrt" "${_gh1_sdk_inc}/um" "${_gh1_sdk_inc}/shared"
        "${_gh1_sdk_inc}/winrt")
    string(APPEND _gh1_includes " /imsvc \"${dir}\"")
endforeach()
# The recompiled EE uses SSE4.1 (x86-64-v2), as the Linux build asks with
# -march. These are the flags' initial values: pass any others with
# CMAKE_<LANG>_FLAGS_INIT, not CMAKE_<LANG>_FLAGS, which would replace them.
set(CMAKE_C_FLAGS_INIT "${_gh1_includes} /clang:-march=x86-64-v2 ${CMAKE_C_FLAGS_INIT}")
set(CMAKE_CXX_FLAGS_INIT "${_gh1_includes} /clang:-march=x86-64-v2 ${CMAKE_CXX_FLAGS_INIT}")

set(_gh1_libpaths "")
foreach(dir "${_gh1_msvc}/lib/x86_64" "${_gh1_sdk_lib}/um/x86_64" "${_gh1_sdk_lib}/ucrt/x86_64")
    string(APPEND _gh1_libpaths " /libpath:\"${dir}\"")
endforeach()
foreach(kind EXE SHARED MODULE)
    set(CMAKE_${kind}_LINKER_FLAGS_INIT "${_gh1_libpaths}")
endforeach()
set(CMAKE_RC_FLAGS_INIT "/I \"${_gh1_sdk_inc}/um\" /I \"${_gh1_sdk_inc}/shared\"")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# xwin brings the release C runtime only; the exe links it statically, so it
# needs no Visual C++ redistributable.
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
