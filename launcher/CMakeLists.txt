string(REPLACE "." "," CJ_VERSION_COMMAS "${PROJECT_VERSION}.0")
configure_file(src/version.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/version.hpp @ONLY)
configure_file(resources/app.rc.in ${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc @ONLY)

add_executable(NorthRayLauncher WIN32
  src/main.cpp
  src/launcher.cpp
  src/jobs.cpp
  src/model.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc
)
target_include_directories(NorthRayLauncher PRIVATE
  src
  ${CMAKE_CURRENT_BINARY_DIR}/generated
  ${CMAKE_SOURCE_DIR}/libs/third_party)
target_link_libraries(NorthRayLauncher PRIVATE cjcore cjui shell32 ole32 dwmapi uuid)

if(MSVC)
  target_compile_options(NorthRayLauncher PRIVATE /W4 /utf-8)
else()
  target_compile_options(NorthRayLauncher PRIVATE -Wall -Wextra -Wno-missing-field-initializers -Wno-cast-function-type)
  # Static runtime so the .exe doesn't need MinGW DLLs alongside it.
  target_link_options(NorthRayLauncher PRIVATE -static -municode)
endif()

# Ship the shared metadata next to the executable.
# A target that always runs, so a metadata change alone is never missed.
add_custom_target(NorthRayLauncherData ALL
  COMMAND ${CMAKE_COMMAND} -E remove_directory $<TARGET_FILE_DIR:NorthRayLauncher>/data
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/shared/metadata $<TARGET_FILE_DIR:NorthRayLauncher>/data
  COMMENT "Copying shared metadata")
add_dependencies(NorthRayLauncherData NorthRayLauncher)
