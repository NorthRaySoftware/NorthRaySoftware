# NorthRay Capture: livestreaming and recording for Windows, in the NorthRay suite. Drawn with cjui
# (Win32 + Direct2D) like the other apps; Direct3D 11 for composition (see docs/ARCHITECTURE.md).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" NCAP_VERSION)
string(STRIP "${NCAP_VERSION}" NCAP_VERSION)
string(REPLACE "." "," NCAP_VERSION_COMMAS "${NCAP_VERSION}.0")

# The portable core: scenes, routing and leak checks, log redaction, layouts. Tested everywhere.
add_library(capturecore STATIC core/src/scenes.cpp core/src/routing.cpp core/src/diag.cpp)
target_include_directories(capturecore PUBLIC core/include PRIVATE ${CMAKE_SOURCE_DIR}/libs/third_party)
if(MSVC)
  target_compile_options(capturecore PRIVATE /W4 /utf-8)
else()
  target_compile_options(capturecore PRIVATE -Wall -Wextra)
endif()

if(CJ_BUILD_TESTS)
  add_executable(capturecore_tests tests/capturecore_tests.cpp)
  target_link_libraries(capturecore_tests PRIVATE capturecore)
  if(MINGW)
    target_link_options(capturecore_tests PRIVATE -static)
  endif()
  add_test(NAME capturecore COMMAND capturecore_tests)
endif()

if(NOT WIN32)
  return()
endif()

configure_file(src/version.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/version.hpp @ONLY)
configure_file(resources/app.rc.in ${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc @ONLY)

add_executable(NorthRayCapture WIN32
  src/main.cpp
  src/capture_window.cpp
  src/settings.cpp
  src/system.cpp
  src/log.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc
)
target_include_directories(NorthRayCapture PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/generated ${CMAKE_SOURCE_DIR}/libs/third_party)
target_link_libraries(NorthRayCapture PRIVATE capturecore cjui cjcore ole32 shell32 comctl32 uuid mfplat mfuuid psapi advapi32 dxgi)
if(MSVC)
  target_compile_options(NorthRayCapture PRIVATE /W4 /utf-8)
else()
  target_compile_options(NorthRayCapture PRIVATE -Wall -Wextra -Wno-missing-field-initializers -Wno-cast-function-type)
  target_link_options(NorthRayCapture PRIVATE -static -municode)
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
