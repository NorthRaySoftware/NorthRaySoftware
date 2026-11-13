string(REPLACE "." "," CJ_VERSION_COMMAS "${PROJECT_VERSION}.0")
configure_file(src/version.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/version.hpp @ONLY)
configure_file(resources/app.rc.in ${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc @ONLY)

add_executable(WritingStudio WIN32
  src/main.cpp
  src/studio_app.cpp
  src/studio_pages.cpp
  src/studio_writing.cpp
  src/studio_artwork.cpp
  src/studio_typeset.cpp
  src/studio_preview.cpp
  src/studio_publish.cpp
  src/studio_world.cpp
  src/studio_plan.cpp
  src/studio_review.cpp
  src/studio_comments.cpp
  src/web_pane.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc
)
target_include_directories(WritingStudio PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/generated ${CMAKE_SOURCE_DIR}/libs/third_party
                           ${CMAKE_SOURCE_DIR}/libs/third_party/webview2)
target_link_libraries(WritingStudio PRIVATE studiocore cjui ole32 shell32 comdlg32 dwmapi uuid comctl32)
if(MSVC)
  target_compile_options(WritingStudio PRIVATE /W4 /utf-8)
else()
  target_compile_options(WritingStudio PRIVATE -Wall -Wextra -Wno-missing-field-initializers -Wno-cast-function-type)
  target_link_options(WritingStudio PRIVATE -static -municode)
endif()

# Package it (verified on install, like the other apps).
add_custom_command(TARGET WritingStudio POST_BUILD
  COMMAND ${CMAKE_COMMAND} -DID=writing-studio -DVERSION=${PROJECT_VERSION} -DEXE=$<TARGET_FILE:WritingStudio>
          -DOUT=${CMAKE_BINARY_DIR}/launcher/packages/writing-studio
          "-DEXTRA_FILES=${CMAKE_SOURCE_DIR}/libs/third_party/webview2/WebView2Loader.dll::${CMAKE_CURRENT_SOURCE_DIR}/third_party/pdfium/pdfium.dll"
          "-DEXTRA_DIRS=${CMAKE_CURRENT_SOURCE_DIR}/web::${CMAKE_CURRENT_SOURCE_DIR}/licenses::${CMAKE_CURRENT_SOURCE_DIR}/fonts::${CMAKE_CURRENT_SOURCE_DIR}/hyphenation"
          -P ${CMAKE_SOURCE_DIR}/cmake/make-package.cmake
  COMMENT "Packaging NorthRay Writing Studio")

# Running WritingStudio.exe straight from the build folder needs the same neighbours.
add_custom_command(TARGET WritingStudio POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/libs/third_party/webview2/WebView2Loader.dll $<TARGET_FILE_DIR:WritingStudio>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pdfium/pdfium.dll $<TARGET_FILE_DIR:WritingStudio>
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/web $<TARGET_FILE_DIR:WritingStudio>/web
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/fonts $<TARGET_FILE_DIR:WritingStudio>/fonts
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/hyphenation $<TARGET_FILE_DIR:WritingStudio>/hyphenation)
