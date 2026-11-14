# NorthRay Files: a file manager for Windows, drawn with cjui like the other
# NorthRay apps (Win32 + Direct2D), using the Windows Shell for icons, types,
# known folders and opening files.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")  # a new version reconfigures
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" NRF_VERSION)
string(STRIP "${NRF_VERSION}" NRF_VERSION)
string(REPLACE "." "," NRF_VERSION_COMMAS "${NRF_VERSION}.0")

# The portable core: paths, sorting, hidden items, selection. Tested everywhere.
add_library(filescore STATIC core/src/paths.cpp core/src/listing.cpp core/src/exif.cpp core/src/operations.cpp core/src/search.cpp core/src/art.cpp core/src/labels.cpp)
target_include_directories(filescore PUBLIC core/include)
target_include_directories(filescore PRIVATE ${CMAKE_SOURCE_DIR}/libs/third_party)
if(MSVC)
  target_compile_options(filescore PRIVATE /W4 /utf-8)
else()
  target_compile_options(filescore PRIVATE -Wall -Wextra)
endif()

if(CJ_BUILD_TESTS)
  add_executable(filescore_tests tests/filescore_tests.cpp)
  target_link_libraries(filescore_tests PRIVATE filescore)
  if(MINGW)
    target_link_options(filescore_tests PRIVATE -static)
  endif()
  add_test(NAME filescore COMMAND filescore_tests)
endif()

if(NOT WIN32)
  return()
endif()

configure_file(src/version.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/version.hpp @ONLY)
configure_file(resources/app.rc.in ${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc @ONLY)

add_executable(NorthRayFiles WIN32
  src/main.cpp
  src/files_window.cpp
  src/shell.cpp
  src/settings.cpp
  src/thumbnails.cpp
  src/operations.cpp
  src/search.cpp
  src/dragdrop.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc
)
target_include_directories(NorthRayFiles PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/generated ${CMAKE_SOURCE_DIR}/libs/third_party)
target_link_libraries(NorthRayFiles PRIVATE filescore miniz cjui cjcore ole32 shell32 comctl32 uuid windowscodecs propsys oleaut32 shlwapi)
if(MSVC)
  target_compile_options(NorthRayFiles PRIVATE /W4 /utf-8)
else()
  target_compile_options(NorthRayFiles PRIVATE -Wall -Wextra -Wno-missing-field-initializers -Wno-cast-function-type)
  target_link_options(NorthRayFiles PRIVATE -static -municode)
endif()

add_custom_command(TARGET NorthRayFiles POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/licenses $<TARGET_FILE_DIR:NorthRayFiles>/licenses)

# Package it (verified on install, like the other apps).
add_custom_command(TARGET NorthRayFiles POST_BUILD
  COMMAND ${CMAKE_COMMAND} -DID=files -DVERSION=${NRF_VERSION} -DEXE=$<TARGET_FILE:NorthRayFiles>
          -DOUT=${CMAKE_BINARY_DIR}/launcher/packages/files
          "-DEXTRA_DIRS=${CMAKE_CURRENT_SOURCE_DIR}/licenses"
          -P ${CMAKE_SOURCE_DIR}/cmake/make-package.cmake
  COMMENT "Packaging NorthRay Files")
