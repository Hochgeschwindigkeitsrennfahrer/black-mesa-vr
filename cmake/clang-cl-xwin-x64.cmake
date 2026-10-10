# Cross-build the x64 OpenXRHelper64.exe on Linux: clang-cl + lld-link against an xwin "splat"
# of the MSVC CRT + Windows SDK (x64). Usage:
#   XWIN=~/Downloads/PSVR2/xwin/sdk cmake -G Ninja -S OpenXRHelper64 -B build-linux-helper \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/clang-cl-xwin-x64.cmake -DCMAKE_BUILD_TYPE=Release
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

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
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)

set(_xwin_inc
  "/imsvc${XWIN}/crt/include"
  "/imsvc${XWIN}/sdk/include/ucrt"
  "/imsvc${XWIN}/sdk/include/um"
  "/imsvc${XWIN}/sdk/include/shared"
  "/imsvc${XWIN}/sdk/include/winrt")
string(JOIN " " _xwin_inc_str ${_xwin_inc})
set(CMAKE_C_FLAGS_INIT "--target=x86_64-pc-windows-msvc -fms-compatibility -fms-extensions ${_xwin_inc_str}")
set(CMAKE_CXX_FLAGS_INIT "--target=x86_64-pc-windows-msvc -fms-compatibility -fms-extensions /EHsc ${_xwin_inc_str}")

set(_xwin_lib "/libpath:${XWIN}/crt/lib/x86_64 /libpath:${XWIN}/sdk/lib/um/x86_64 /libpath:${XWIN}/sdk/lib/ucrt/x86_64")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_xwin_lib}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_xwin_lib}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_xwin_lib}")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
