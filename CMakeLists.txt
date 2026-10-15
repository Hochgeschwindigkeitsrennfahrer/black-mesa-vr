cmake_minimum_required(VERSION 3.20)
project(BMVR LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Black Mesa is Win32. Configure with: cmake -A Win32 -B build")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(DXVK "${ROOT}/third_party/l4d2vr/dxvk_new")
set(SRC_LIST "${ROOT}/tools/dxvk_sources.txt")

if(NOT EXISTS "${SRC_LIST}")
  message(FATAL_ERROR "Missing ${SRC_LIST}")
endif()

file(STRINGS "${SRC_LIST}" DXVK_REL_SOURCES)
set(DXVK_SOURCES "")
foreach(rel ${DXVK_REL_SOURCES})
  string(STRIP "${rel}" rel)
  if(rel STREQUAL "")
    continue()
  endif()
  list(APPEND DXVK_SOURCES "${DXVK}/${rel}")
endforeach()

set(BMVR_SOURCES
  src/dllmain.cpp
  src/game.cpp
  src/hooks.cpp
  src/vr.cpp
  src/vr_audio_focus.cpp
  src/vr_crowbar.cpp
  src/vr_weapon_menu.cpp
  src/vr_hands.cpp
  src/vr_hand_renderer_d3d9.cpp
  src/vr_runtime_backend.cpp
  src/openxr_helper_bridge.cpp
  src/vr_fake_helper.cpp
  src/openxr_loader_util.cpp
  src/bmvr_flags.cpp
  src/bmvr_cvars.cpp
  src/sdk/checksum_crc.cpp
  third_party/l4d2vr/L4D2VR/vr_hands/vr_hand_asset_loader.cpp
  third_party/minhook/src/buffer.c
  third_party/minhook/src/hook.c
  third_party/minhook/src/trampoline.c
  third_party/minhook/src/hde/hde32.c
)

add_library(d3d9 SHARED ${DXVK_SOURCES} ${BMVR_SOURCES})
set_target_properties(d3d9 PROPERTIES
  OUTPUT_NAME "d3d9"
  PREFIX ""
  WINDOWS_EXPORT_ALL_SYMBOLS OFF
)

target_compile_definitions(d3d9 PRIVATE
  WIN32
  _WINDOWS
  _USRDLL
  NOMINMAX
  DXVK_WSI_WIN32
  _CRT_SECURE_NO_WARNINGS
  $<$<CONFIG:Release>:NDEBUG>
)

target_include_directories(d3d9 PRIVATE
  "${ROOT}"
  "${ROOT}/src"
  "${ROOT}/src/sdk"
  "${ROOT}/L4D2VR"
  "${ROOT}/third_party/l4d2vr/L4D2VR/vr_hands"
  "${ROOT}/third_party/l4d2vr/thirdparty/cgltf"
  "${DXVK}"
  "${DXVK}/include"
  "${DXVK}/include/vulkan/include"
  "${DXVK}/include/spirv/include"
  "${DXVK}/include/shaders"
  "${DXVK}/src"
  "${DXVK}/src/dxvk"
  "${DXVK}/src/d3d9"
  "${DXVK}/src/d3d9/shaders"
  "${DXVK}/subprojects/libdisplay-info"
  "${DXVK}/subprojects/libdisplay-info/include"
  "${ROOT}/third_party/minhook/include"
  "${ROOT}/third_party/openvr/include"
  "${ROOT}/third_party/openxr/include"
)

target_compile_options(d3d9 PRIVATE
  /MP
  /bigobj
  /W3
  /wd4244
  /wd4267
  /wd4018
  /wd4996
  /wd4305
  /wd4100
  /wd4458
  /wd4456
  /Zc:__cplusplus
)

target_link_directories(d3d9 PRIVATE
  "${ROOT}/third_party/openvr/lib/win32"
  "${DXVK}/lib32"
)

target_link_libraries(d3d9 PRIVATE
  openvr_api
  vulkan-1
  synchronization
  setupapi
  psapi
  winmm
  shell32
  ole32
  windowscodecs
  gdi32
  user32
  advapi32
)

set_target_properties(d3d9 PROPERTIES
  LINK_FLAGS "/DEF:\"${DXVK}/src/d3d9/d3d9.def\" /INCREMENTAL:NO"
)

if(MSVC)
  set_source_files_properties(
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde32.c
    PROPERTIES LANGUAGE C
  )
  set(LIBDISPLAY_C
    "${DXVK}/subprojects/libdisplay-info/cta.c"
    "${DXVK}/subprojects/libdisplay-info/displayid.c"
    "${DXVK}/subprojects/libdisplay-info/dmt-table.c"
    "${DXVK}/subprojects/libdisplay-info/edid.c"
    "${DXVK}/subprojects/libdisplay-info/gtf.c"
    "${DXVK}/subprojects/libdisplay-info/info.c"
    "${DXVK}/subprojects/libdisplay-info/log.c"
    "${DXVK}/subprojects/libdisplay-info/memory-stream.c"
    "${DXVK}/subprojects/libdisplay-info/pnp-id-table.c"
  )
  set_source_files_properties(${LIBDISPLAY_C} PROPERTIES
    LANGUAGE C
    COMPILE_DEFINITIONS "static_array=;ssize_t=intptr_t"
  )
endif()
