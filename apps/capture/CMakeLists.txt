# NorthRay Capture: livestreaming and recording for Windows, in the NorthRay suite. Drawn with cjui
# (Win32 + Direct2D) like the other apps; Direct3D 11 for composition (see docs/ARCHITECTURE.md).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" NCAP_VERSION)
string(STRIP "${NCAP_VERSION}" NCAP_VERSION)
string(REPLACE "." "," NCAP_VERSION_COMMAS "${NCAP_VERSION}.0")

# The portable core: scenes, routing and leak checks, log redaction, layouts. Tested everywhere.
add_library(capturecore STATIC core/src/scenes.cpp core/src/placement.cpp core/src/audio.cpp core/src/media.cpp core/src/mkv.cpp core/src/rtmp.cpp core/src/oauth.cpp core/src/transition.cpp core/src/sourceutil.cpp core/src/border.cpp core/src/design.cpp core/src/chat.cpp core/src/mp4.cpp core/src/profiles.cpp core/src/wizard.cpp core/src/soundboard.cpp core/src/keying.cpp core/src/gif.cpp core/src/checklist.cpp core/src/automation.cpp core/src/hotkeys.cpp core/src/aurora_sky.cpp core/src/scene_templates.cpp core/src/visualiser.cpp ${CMAKE_SOURCE_DIR}/libs/third_party/qrcodegen/qrcodegen.cpp core/src/dsp.cpp core/src/plugins.cpp core/src/routing.cpp core/src/diag.cpp core/src/live.cpp core/src/pointer.cpp)
target_include_directories(capturecore PUBLIC core/include PRIVATE ${CMAKE_SOURCE_DIR}/libs/third_party)
if(MSVC)
  target_compile_options(capturecore PRIVATE /W4 /utf-8)
else()
  target_compile_options(capturecore PRIVATE -Wall -Wextra)
endif()

if(CJ_BUILD_TESTS)
  add_executable(capturecore_tests tests/capturecore_tests.cpp)
  target_link_libraries(capturecore_tests PRIVATE capturecore)
  target_include_directories(capturecore_tests PRIVATE ${CMAKE_SOURCE_DIR}/libs/third_party)
  if(MINGW)
    target_link_options(capturecore_tests PRIVATE -static)
  endif()
  add_test(NAME capturecore COMMAND capturecore_tests)
  # Not a test by itself: builds an MKV from an H.264 and AAC elementary stream, for checking
  # with ffprobe/ffmpeg (tools/check-mkv.sh).
  if(NOT WIN32)
    add_executable(capture_mkv_check tests/mkv_check.cpp)
    target_link_libraries(capture_mkv_check PRIVATE capturecore)
    add_executable(capture_rtmp_check tests/rtmp_check.cpp)
    target_link_libraries(capture_rtmp_check PRIVATE capturecore)
  endif()
endif()

if(NOT WIN32)
  return()
endif()

configure_file(src/version.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/version.hpp @ONLY)
# What's new (0.10.32): this version's release notes, from the suite's metadata, shown once after an update.
set(NCAP_RELEASES "${CMAKE_SOURCE_DIR}/shared/metadata/releases/capture.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NCAP_RELEASES}")
file(READ "${NCAP_RELEASES}" NCAP_RELEASES_JSON)
set(NCAP_WHATSNEW "")
string(JSON NCAP_NOTES_VERSION ERROR_VARIABLE NCAP_JSON_ERR GET "${NCAP_RELEASES_JSON}" releases 0 version)
if(NOT NCAP_JSON_ERR AND NCAP_NOTES_VERSION STREQUAL NCAP_VERSION)
  string(JSON NCAP_NOTES_N LENGTH "${NCAP_RELEASES_JSON}" releases 0 notes)
  math(EXPR NCAP_NOTES_LAST "${NCAP_NOTES_N} - 1")
  foreach(i RANGE 0 ${NCAP_NOTES_LAST})
    string(JSON NCAP_NOTE GET "${NCAP_RELEASES_JSON}" releases 0 notes ${i})
    string(REPLACE "\\" "\\\\" NCAP_NOTE "${NCAP_NOTE}")
    string(REPLACE "\"" "\\\"" NCAP_NOTE "${NCAP_NOTE}")
    string(APPEND NCAP_WHATSNEW "    L\"${NCAP_NOTE}\",\n")
  endforeach()
endif()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/whatsnew.hpp.new
     "#pragma once\n// This version's release notes (made by CMake from shared/metadata/releases/capture.json).\ninline const wchar_t* const kWhatsNew[] = {\n${NCAP_WHATSNEW}    nullptr,\n};\n")
configure_file(${CMAKE_CURRENT_BINARY_DIR}/generated/whatsnew.hpp.new ${CMAKE_CURRENT_BINARY_DIR}/generated/whatsnew.hpp COPYONLY)
configure_file(resources/app.rc.in ${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc @ONLY)

add_executable(NorthRayCapture WIN32
  src/main.cpp
  src/capture_window.cpp
  src/capture_sources.cpp
  src/capture_audio.cpp
  src/capture_record.cpp
  src/capture_stream.cpp
  src/capture_studio.cpp
  src/capture_made.cpp
  src/capture_filters.cpp src/capture_border.cpp src/capture_plugins.cpp src/capture_designer.cpp src/capture_brand.cpp src/capture_profiles.cpp src/capture_wizard.cpp src/capture_checklist.cpp src/capture_live.cpp src/capture_notes.cpp src/capture_destview.cpp src/capture_setuptest.cpp src/capture_automation.cpp src/capture_hotkeys.cpp src/capture_scene_templates.cpp src/capture_library.cpp src/capture_soundboard.cpp src/capture_key.cpp src/audio/plugin_host.cpp
  src/output/net.cpp
  src/output/twitch_chat.cpp
  src/output/mp4_copy.cpp
  src/audio/soundboard_player.cpp
  src/output/credentials.cpp
  src/output/destination.cpp
  src/output/streamer.cpp
  src/encode/video_encoder.cpp
  src/encode/audio_encoder.cpp
  src/output/recorder.cpp
  src/audio/devices.cpp
  src/audio/capture.cpp
  src/audio/mixer.cpp
  src/video/gpu.cpp
  src/video/wgc.cpp
  src/video/devices.cpp
  src/video/sources.cpp
  src/video/made_sources.cpp src/video/design_render.cpp
  src/video/media_source.cpp
  src/video/engine.cpp
  src/settings.cpp
  src/system.cpp
  src/log.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc
)
target_include_directories(NorthRayCapture PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/generated ${CMAKE_SOURCE_DIR}/libs/third_party)
target_link_libraries(NorthRayCapture PRIVATE capturecore cjui cjcore ole32 shell32 comctl32 uuid mfplat mfuuid psapi advapi32 dxgi d3d11 dwmapi mf mfreadwrite winmm mmdevapi version mfreadwrite ws2_32 secur32 crypt32 d2d1 dwrite windowscodecs propsys wintrust winhttp)
if(MSVC)
  target_compile_options(NorthRayCapture PRIVATE /W4 /utf-8)
else()
  target_compile_options(NorthRayCapture PRIVATE -Wall -Wextra -Wno-missing-field-initializers -Wno-cast-function-type)
  target_link_options(NorthRayCapture PRIVATE -static -municode)
  # Not a test by itself: the MP4 copy on an MKV (tests/mp4_copy_check.cpp).
  add_executable(capture_mp4_copy_check tests/mp4_copy_check.cpp src/output/mp4_copy.cpp src/log.cpp)
  target_include_directories(capture_mp4_copy_check PRIVATE src)
  target_link_libraries(capture_mp4_copy_check PRIVATE capturecore cjcore)
  target_link_options(capture_mp4_copy_check PRIVATE -static -municode)
  # Not a test by itself: every design template drawn to PNGs with sample details (tests/design_render_check.cpp).
  add_executable(capture_design_render_check tests/design_render_check.cpp src/video/design_render.cpp)
  target_include_directories(capture_design_render_check PRIVATE src)
  target_link_libraries(capture_design_render_check PRIVATE capturecore cjcore ole32 d2d1 dwrite windowscodecs uuid)
  target_link_options(capture_design_render_check PRIVATE -static -municode)
endif()

add_custom_command(TARGET NorthRayCapture POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/licenses $<TARGET_FILE_DIR:NorthRayCapture>/licenses)

# Package it (verified on install, like the other apps).
add_custom_command(TARGET NorthRayCapture POST_BUILD
  COMMAND ${CMAKE_COMMAND} -DID=capture -DVERSION=${NCAP_VERSION} -DEXE=$<TARGET_FILE:NorthRayCapture>
          -DOUT=${CMAKE_BINARY_DIR}/launcher/packages/capture
          "-DEXTRA_DIRS=${CMAKE_CURRENT_SOURCE_DIR}/licenses"
          -P ${CMAKE_SOURCE_DIR}/cmake/make-package.cmake
  COMMENT "Packaging NorthRay Capture")
