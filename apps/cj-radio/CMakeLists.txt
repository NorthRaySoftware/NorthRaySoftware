string(REPLACE "." "," CJ_VERSION_COMMAS "${PROJECT_VERSION}.0")
configure_file(src/version.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/version.hpp @ONLY)
configure_file(resources/app.rc.in ${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc @ONLY)

add_executable(CJRadio WIN32
  src/main.cpp
  src/radio_app.cpp
  src/player.cpp
  src/twitch_view.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc
)
target_include_directories(CJRadio PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/generated ${CMAKE_SOURCE_DIR}/libs/third_party ${CMAKE_SOURCE_DIR}/libs/third_party/webview2)
target_link_libraries(CJRadio PRIVATE cjcore cjui ole32 winmm shell32)
if(MSVC)
  target_compile_options(CJRadio PRIVATE /W4 /utf-8)
else()
  target_compile_options(CJRadio PRIVATE -Wall -Wextra -Wno-missing-field-initializers -Wno-cast-function-type -Wno-unused-function)
  target_link_options(CJRadio PRIVATE -static -municode)
  # miniaudio's implementation is large; keep its own warnings out of ours.
  set_source_files_properties(src/player.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()

# Package it for the launcher (verified on install).
add_custom_command(TARGET CJRadio POST_BUILD
  COMMAND ${CMAKE_COMMAND} -DID=cj-radio -DVERSION=${PROJECT_VERSION} -DEXE=$<TARGET_FILE:CJRadio>
          -DDATA=${CMAKE_SOURCE_DIR}/shared/metadata -DOUT=${CMAKE_BINARY_DIR}/launcher/packages/cj-radio
          "-DEXTRA_FILES=${CMAKE_SOURCE_DIR}/libs/third_party/webview2/WebView2Loader.dll"
          "-DEXTRA_DIRS=${CMAKE_CURRENT_SOURCE_DIR}/web::${CMAKE_CURRENT_SOURCE_DIR}/Recordings::${CMAKE_CURRENT_SOURCE_DIR}/licenses"
          -P ${CMAKE_SOURCE_DIR}/cmake/make-package.cmake
  COMMENT "Packaging CJ Radio")

# Running CJRadio.exe straight from the build folder needs the same neighbours.
add_custom_command(TARGET CJRadio POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/libs/third_party/webview2/WebView2Loader.dll $<TARGET_FILE_DIR:CJRadio>
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/web $<TARGET_FILE_DIR:CJRadio>/web
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/Recordings $<TARGET_FILE_DIR:CJRadio>/Recordings)
