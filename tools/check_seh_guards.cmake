# Fails the build on a bare __try in src/. Use BMVR_TRY (src/bmvr_seh.h): on the
# 32-bit clang-cl build a caught fault otherwise returns with EBX/ESI/EDI wiped.
# A deliberate bare __try needs "seh-lint: allow" on the same line.
#   cmake -DSRC_DIR=<repo>/src -P tools/check_seh_guards.cmake
if(NOT SRC_DIR)
  message(FATAL_ERROR "check_seh_guards: SRC_DIR not set")
endif()

file(GLOB_RECURSE _files "${SRC_DIR}/*.cpp" "${SRC_DIR}/*.h" "${SRC_DIR}/*.hpp" "${SRC_DIR}/*.c")
set(_bad "")
foreach(_f ${_files})
  file(READ "${_f}" _text)
  if(NOT _text MATCHES "__try")
    continue()
  endif()
  string(REGEX REPLACE "[^\n]*seh-lint: allow[^\n]*" "" _text "${_text}")
  string(REGEX REPLACE "//[^\n]*" "" _text "${_text}")
  string(REGEX REPLACE "#define BMVR_TRY[^\n]*" "" _text "${_text}")
  string(REGEX MATCHALL "(^|[^A-Za-z0-9_])__try([^A-Za-z0-9_]|$)" _hits "${_text}")
  list(LENGTH _hits _n)
  if(_n GREATER 0)
    file(RELATIVE_PATH _rel "${SRC_DIR}/.." "${_f}")
    string(APPEND _bad "  ${_rel}: ${_n}\n")
  endif()
endforeach()

if(_bad)
  message(FATAL_ERROR "Bare __try found (use BMVR_TRY from src/bmvr_seh.h):\n${_bad}")
endif()
