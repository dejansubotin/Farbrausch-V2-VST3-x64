cmake_minimum_required(VERSION 3.25)

project(V2 VERSION 0.1.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(V2_ENABLE_RONAN "Enable the Ronan speech synthesizer" ON)
option(V2_BUILD_STANDALONE "Build the x64 standalone synth" ON)
option(V2_BUILD_RECORD_SMOKE "Build the V2M record smoke test" ON)
option(V2_BUILD_VST3 "Build the x64 VST3 plugin" ON)
option(V2_BUILD_V2M_PLAYER "Build the x64 V2M player app" ON)

set(V2_WTL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../deps/WTL-main" CACHE PATH "Path to a WTL checkout")
set(V2_VST3_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../deps/vst3sdk-full/VST_SDK/vst3sdk" CACHE PATH "Path to the full Steinberg VST3 SDK")

if((V2_BUILD_STANDALONE OR V2_BUILD_VST3) AND NOT EXISTS "${V2_WTL_DIR}/Include/atlapp.h")
  message(FATAL_ERROR "WTL headers not found at ${V2_WTL_DIR}. Set V2_WTL_DIR or disable the targets that require it.")
endif()

if(V2_BUILD_VST3)
  if(NOT EXISTS "${V2_VST3_SDK_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Full VST3 SDK not found at ${V2_VST3_SDK_DIR}. Set V2_VST3_SDK_DIR or disable V2_BUILD_VST3.")
  endif()

  set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
  set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)
  set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "" FORCE)
  set(SMTG_CREATE_MODULE_INFO OFF CACHE BOOL "" FORCE)

  add_subdirectory("${V2_VST3_SDK_DIR}" "${CMAKE_BINARY_DIR}/vst3sdk" EXCLUDE_FROM_ALL)
  set(SDK_ROOT "${V2_VST3_SDK_DIR}")
endif()

if(V2_BUILD_STANDALONE)
add_executable(v2standalone_x64 WIN32
  dsio_cpp.cpp
  e2p_stub.cpp
  ronan.cpp
  sounddef.cpp
  synth_core.cpp
  tool/file.cpp
  v2mconv.cpp
  vsti/v2mrecorder.cpp
  vsti/wtlbla.rc
  x64_standalone/main.cpp
  x64_standalone/midi_backend.cpp
)

target_include_directories(v2standalone_x64 PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/tool"
  "${CMAKE_CURRENT_SOURCE_DIR}/vsti"
  "${V2_WTL_DIR}/Include"
)

target_compile_definitions(v2standalone_x64 PRIVATE
  _MBCS
  WIN32_LEAN_AND_MEAN
  _CRT_SECURE_NO_DEPRECATE
  _CRT_NON_CONFORMING_SWPRINTFS
  _WTL_NO_CSTRING
  WINVER=0x0601
  _WIN32_IE=0x0601
  _RICHEDIT_VER=0x0200
  $<$<BOOL:${V2_ENABLE_RONAN}>:RONAN>
)

target_link_libraries(v2standalone_x64 PRIVATE
  comctl32
  dsound
  gdiplus
  ole32
  winmm
)

if(MSVC)
  target_compile_options(v2standalone_x64 PRIVATE
    /EHsc
    /MP
    /W3
    /wd4018
    /wd4244
    /wd4267
    /wd4305
    /wd4996
  )
  set_property(TARGET v2standalone_x64 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  set_property(TARGET v2standalone_x64 PROPERTY OUTPUT_NAME "Farbrausch V2 x64")
endif()

add_custom_command(TARGET v2standalone_x64 POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/presets.v2b"
    "$<TARGET_FILE_DIR:v2standalone_x64>/presets.v2b"
)
endif()

if(V2_BUILD_RECORD_SMOKE)
add_executable(v2record_smoke
  dsio_cpp.cpp
  e2p_stub.cpp
  ronan.cpp
  sounddef.cpp
  synth_core.cpp
  tool/file.cpp
  v2mconv.cpp
  vsti/v2mrecorder.cpp
  x64_standalone/midi_backend.cpp
  x64_standalone/record_smoke.cpp
)

target_include_directories(v2record_smoke PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/tool"
  "${CMAKE_CURRENT_SOURCE_DIR}/vsti"
)

target_compile_definitions(v2record_smoke PRIVATE
  _MBCS
  WIN32_LEAN_AND_MEAN
  _CRT_SECURE_NO_DEPRECATE
  _CRT_NON_CONFORMING_SWPRINTFS
  WINVER=0x0601
  _WIN32_IE=0x0601
  _RICHEDIT_VER=0x0200
  $<$<BOOL:${V2_ENABLE_RONAN}>:RONAN>
)

target_link_libraries(v2record_smoke PRIVATE
  dsound
  winmm
)

if(MSVC)
  target_compile_options(v2record_smoke PRIVATE
    /EHsc
    /MP
    /W3
    /wd4018
    /wd4244
    /wd4267
    /wd4305
    /wd4996
  )
  set_property(TARGET v2record_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
endif()

if(V2_BUILD_V2M_PLAYER)
add_executable(v2m_player_x64 WIN32
  dsio_cpp.cpp
  e2p_stub.cpp
  ronan.cpp
  sounddef.cpp
  synth_core.cpp
  tool/file.cpp
  v2mconv.cpp
  v2mplayer.cpp
  v2m_player_x64/main.cpp
)

target_include_directories(v2m_player_x64 PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/tool"
)

target_compile_definitions(v2m_player_x64 PRIVATE
  _MBCS
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _CRT_SECURE_NO_DEPRECATE
  _CRT_NON_CONFORMING_SWPRINTFS
  WINVER=0x0601
  _WIN32_IE=0x0601
  $<$<BOOL:${V2_ENABLE_RONAN}>:RONAN>
)

target_link_libraries(v2m_player_x64 PRIVATE
  comdlg32
  dsound
  gdiplus
  ole32
  shell32
)

if(MSVC)
  target_compile_options(v2m_player_x64 PRIVATE
    /EHsc
    /MP
    /W3
    /wd4018
    /wd4244
    /wd4267
    /wd4305
    /wd4996
  )
  set_property(TARGET v2m_player_x64 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  set_property(TARGET v2m_player_x64 PROPERTY OUTPUT_NAME "Farbrausch V2M Player x64")
endif()
endif()

if(V2_BUILD_VST3)
add_library(FarbrauschV2Helper SHARED
  e2p_stub.cpp
  ronan.cpp
  sounddef.cpp
  synth_core.cpp
  tool/file.cpp
  v2mconv.cpp
  vsti/v2mrecorder.cpp
  vsti/wtlbla.rc
  vst3/helper_backend.cpp
  vst3/helper_main.cpp
)

target_include_directories(FarbrauschV2Helper PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/tool"
  "${CMAKE_CURRENT_SOURCE_DIR}/vsti"
  "${V2_WTL_DIR}/Include"
)

target_compile_definitions(FarbrauschV2Helper PRIVATE
  _MBCS
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _CRT_SECURE_NO_DEPRECATE
  _CRT_NON_CONFORMING_SWPRINTFS
  _WTL_NO_CSTRING
  WINVER=0x0601
  _WIN32_IE=0x0601
  _RICHEDIT_VER=0x0200
  $<$<BOOL:${V2_ENABLE_RONAN}>:RONAN>
)

target_link_libraries(FarbrauschV2Helper PRIVATE
  comctl32
  gdiplus
  ole32
)

if(MSVC)
  target_compile_options(FarbrauschV2Helper PRIVATE
    /EHsc
    /MP
    /W3
    /wd4018
    /wd4244
    /wd4267
    /wd4305
    /wd4996
  )
  set_property(TARGET FarbrauschV2Helper PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set_target_properties(FarbrauschV2Helper PROPERTIES
  OUTPUT_NAME "FarbrauschV2Helper"
)

smtg_add_vst3plugin(farbrausch_v2_vst3
  PACKAGE_NAME "Farbrausch V2"
  SOURCES_LIST
    "${SDK_ROOT}/public.sdk/source/main/dllmain.cpp"
    "${SDK_ROOT}/public.sdk/source/vst/vstsinglecomponenteffect.cpp"
    vst3/v2vst3_plugin.cpp
)

target_link_libraries(farbrausch_v2_vst3 PRIVATE
  sdk
)

target_compile_features(farbrausch_v2_vst3 PRIVATE
  cxx_std_17
)

target_compile_definitions(farbrausch_v2_vst3 PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
)

add_dependencies(farbrausch_v2_vst3 moduleinfotool)
get_target_property(FARBRAUSCH_V2_VST3_PACKAGE_PATH farbrausch_v2_vst3 SMTG_PLUGIN_PACKAGE_PATH)

if(MSVC)
  target_compile_options(farbrausch_v2_vst3 PRIVATE
    /EHsc
    /MP
    /W3
    /wd4244
    /wd4267
    /wd4996
  )
  set_property(TARGET farbrausch_v2_vst3 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

add_custom_command(TARGET farbrausch_v2_vst3 POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_FILE:FarbrauschV2Helper>"
    "$<TARGET_FILE_DIR:farbrausch_v2_vst3>/FarbrauschV2Helper.dll"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/presets.v2b"
    "$<TARGET_FILE_DIR:farbrausch_v2_vst3>/presets.v2b"
  COMMAND $<TARGET_FILE:moduleinfotool> -create -version ${PROJECT_VERSION}
    -path "${FARBRAUSCH_V2_VST3_PACKAGE_PATH}"
    -output "${FARBRAUSCH_V2_VST3_PACKAGE_PATH}/Contents/Resources/moduleinfo.json"
)
endif()
