# NorthRay Browser: a desktop web browser with creative tools, on Microsoft's
# WebView2 engine. Built like the other NorthRay apps (MinGW static, or MSVC).
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" NRB_VERSION)
string(STRIP "${NRB_VERSION}" NRB_VERSION)
string(REPLACE "." "," NRB_VERSION_COMMAS "${NRB_VERSION}.0")

# The portable core: address bar rules, URLs, error text, tabs. No Windows or
# engine code, so it's unit-tested on every platform.
add_library(browsercore STATIC
  core/src/omnibox.cpp
  core/src/url.cpp
  core/src/load_errors.cpp
  core/src/tab_model.cpp
)
target_include_directories(browsercore PUBLIC core/include)
if(MSVC)
  target_compile_options(browsercore PRIVATE /W4 /utf-8)
else()
  target_compile_options(browsercore PRIVATE -Wall -Wextra)
endif()

if(CJ_BUILD_TESTS)
  add_executable(browsercore_tests tests/browsercore_tests.cpp)
  target_link_libraries(browsercore_tests PRIVATE browsercore)
  if(MINGW)
    target_link_options(browsercore_tests PRIVATE -static)
  endif()
  add_test(NAME browsercore COMMAND browsercore_tests)
endif()

if(NOT WIN32)
  return()
endif()

configure_file(src/version.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/version.hpp @ONLY)
configure_file(resources/app.rc.in ${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc @ONLY)

set(NRB_WEBVIEW2 ${CMAKE_SOURCE_DIR}/libs/third_party/webview2-sdk)

add_executable(NorthRayBrowser WIN32
  src/main.cpp
  src/engine.cpp
  src/browser_window.cpp
  src/settings.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc
)
target_include_directories(NorthRayBrowser PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/generated
                           ${CMAKE_SOURCE_DIR}/libs/third_party ${NRB_WEBVIEW2})
target_link_libraries(NorthRayBrowser PRIVATE browsercore cjui cjcore ole32 shell32 comctl32 dwmapi uuid windowscodecs)
if(MSVC)
  target_compile_options(NorthRayBrowser PRIVATE /W4 /utf-8)
else()
  target_compile_options(NorthRayBrowser PRIVATE -Wall -Wextra -Wno-missing-field-initializers -Wno-cast-function-type)
  target_link_options(NorthRayBrowser PRIVATE -static -municode)
endif()

# Running NorthRayBrowser.exe from the build folder needs the loader beside it.
add_custom_command(TARGET NorthRayBrowser POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${NRB_WEBVIEW2}/WebView2Loader.dll $<TARGET_FILE_DIR:NorthRayBrowser>
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/licenses $<TARGET_FILE_DIR:NorthRayBrowser>/licenses)

# Package it (verified on install, like the other apps). Not yet listed in
# shared/metadata/apps, so the Launcher doesn't offer it until it's released.
add_custom_command(TARGET NorthRayBrowser POST_BUILD
  COMMAND ${CMAKE_COMMAND} -DID=browser -DVERSION=${NRB_VERSION} -DEXE=$<TARGET_FILE:NorthRayBrowser>
          -DOUT=${CMAKE_BINARY_DIR}/launcher/packages/browser
          "-DEXTRA_FILES=${NRB_WEBVIEW2}/WebView2Loader.dll"
          "-DEXTRA_DIRS=${CMAKE_CURRENT_SOURCE_DIR}/licenses"
          -P ${CMAKE_SOURCE_DIR}/cmake/make-package.cmake
  COMMENT "Packaging NorthRay Browser")
