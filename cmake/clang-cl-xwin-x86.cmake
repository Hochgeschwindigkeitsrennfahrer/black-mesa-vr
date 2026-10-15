# Cross-build the Win32 d3d9.dll on Linux: clang-cl + lld-link against an xwin "splat"
# of the MSVC CRT + Windows SDK (x86). Usage:
#   XWIN=/path/to/xwin/splat cmake -G Ninja -B build-linux \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/clang-cl-xwin-x86.cmake -DCMAKE_BUILD_TYPE=Release
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR X86)

if(NOT DEFINED ENV{XWIN})
  message(FATAL_ERROR "set XWIN to the xwin splat output directory")
endif()
set(XWIN "$ENV{XWIN}")

set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_LINKER lld-link)
set(CMAKE_AR llvm-lib)
set(CMAKE_RC_COMPILER llvm-rc)
set(CMAKE_MT llvm-mt)
set(CMAKE_C_COMPILER_TARGET i686-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET i686-pc-windows-msvc)

set(_xwin_inc
  "/imsvc${XWIN}/crt/include"
  "/imsvc${XWIN}/sdk/include/ucrt"
  "/imsvc${XWIN}/sdk/include/um"
  "/imsvc${XWIN}/sdk/include/shared"
  "/imsvc${XWIN}/sdk/include/winrt")
string(JOIN " " _xwin_inc_str ${_xwin_inc})
set(CMAKE_C_FLAGS_INIT "--target=i686-pc-windows-msvc -fms-compatibility -fms-extensions ${_xwin_inc_str}")
set(CMAKE_CXX_FLAGS_INIT "--target=i686-pc-windows-msvc -fms-compatibility -fms-extensions /EHa ${_xwin_inc_str}")

set(_xwin_lib "/libpath:${XWIN}/crt/lib/x86 /libpath:${XWIN}/sdk/lib/um/x86 /libpath:${XWIN}/sdk/lib/ucrt/x86")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_xwin_lib}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_xwin_lib}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_xwin_lib}")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
# The mod guards raw game-memory reads with __try/__except. clang only catches
# a fault inside the __try body itself with /EHa (async SEH); CMake's MSVC
# defaults append /EHsc to CMAKE_CXX_FLAGS, so put /EHa in the per-config
# flags, which come later on the command line. Even with /EHa, clang (23, x86)
# resets the try-state to -1 at the lifetime start of any local declared inside
# the __try body, so the guarded loads ran unprotected (ServerEntityIndex
# crash on map load, 2026-10-11); -disable-lifetime-markers keeps them inside.
set(_bmvr_seh "/EHa -Xclang -disable-lifetime-markers")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "${_bmvr_seh}")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO_INIT "${_bmvr_seh}")
set(CMAKE_CXX_FLAGS_DEBUG_INIT "${_bmvr_seh}")
