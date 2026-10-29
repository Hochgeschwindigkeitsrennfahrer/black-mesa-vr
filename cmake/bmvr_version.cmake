# Writes bmvr_version.h (run at every build, file only touched on change).
#   cmake -DSRC=<repo> -DOUT=<header> -P bmvr_version.cmake
# Release name from the BMVR_RELEASE environment variable (set by the release
# script), else "dev build"; commit, its date and a -dirty mark from git.
set(release "$ENV{BMVR_RELEASE}")
if(release STREQUAL "")
  set(release "dev build")
endif()
set(commit "unknown")
set(date "")
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SRC}" rev-parse --short=7 HEAD
    OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0 OR commit STREQUAL "")
    set(commit "unknown")
  else()
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SRC}" log -1 --format=%cs
      OUTPUT_VARIABLE date OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SRC}" status --porcelain --untracked-files=no
      OUTPUT_VARIABLE dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT dirty STREQUAL "")
      string(APPEND commit "-dirty")
    endif()
  endif()
endif()
set(text "#pragma once\n#define BMVR_RELEASE \"${release}\"\n#define BMVR_COMMIT \"${commit}\"\n#define BMVR_COMMIT_DATE \"${date}\"\n")
if(EXISTS "${OUT}")
  file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
  file(WRITE "${OUT}" "${text}")
endif()
