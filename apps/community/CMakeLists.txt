string(REPLACE "." "," CJ_VERSION_COMMAS "${PROJECT_VERSION}.0")
configure_file(src/version.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/version.hpp @ONLY)
configure_file(resources/app.rc.in ${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc @ONLY)

add_executable(CJPhilipCommunity WIN32
  src/main.cpp
  src/hub.cpp
  # Installs and updates: the same catalogue, checks and silent setups as NorthRay Launcher.
  ${CMAKE_SOURCE_DIR}/launcher/src/model.cpp
  ${CMAKE_SOURCE_DIR}/launcher/src/jobs.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc
)
target_include_directories(CJPhilipCommunity PRIVATE
  src
  ${CMAKE_SOURCE_DIR}/launcher/src
  ${CMAKE_CURRENT_BINARY_DIR}/generated
  ${CMAKE_SOURCE_DIR}/libs/third_party)
target_link_libraries(CJPhilipCommunity PRIVATE cjcore cjui shell32 ole32 uuid)

if(MSVC)
  target_compile_options(CJPhilipCommunity PRIVATE /W4 /utf-8)
else()
  target_compile_options(CJPhilipCommunity PRIVATE -Wall -Wextra -Wno-missing-field-initializers -Wno-cast-function-type)
  # Static runtime so the .exe doesn't need MinGW DLLs alongside it.
  target_link_options(CJPhilipCommunity PRIVATE -static -municode)
endif()

# Package it like the other apps (with the metadata it reads).
add_custom_command(TARGET CJPhilipCommunity POST_BUILD
  COMMAND ${CMAKE_COMMAND} -DID=cj-philip-community -DVERSION=${PROJECT_VERSION} -DEXE=$<TARGET_FILE:CJPhilipCommunity>
          -DDATA=${CMAKE_SOURCE_DIR}/shared/metadata -DOUT=${CMAKE_BINARY_DIR}/launcher/packages/cj-philip-community
          -P ${CMAKE_SOURCE_DIR}/cmake/make-package.cmake
  COMMENT "Packaging CJ Philip Community")

# Ship the shared metadata next to the executable.
add_custom_command(TARGET CJPhilipCommunity POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E remove_directory $<TARGET_FILE_DIR:CJPhilipCommunity>/data
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/shared/metadata $<TARGET_FILE_DIR:CJPhilipCommunity>/data
  COMMENT "Copying shared metadata")
