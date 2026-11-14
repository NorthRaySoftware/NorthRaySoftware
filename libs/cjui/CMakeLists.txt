# Shared Windows UI: Direct2D drawing, the window base and controls, formatting.
add_library(cjui STATIC src/gfx.cpp src/window.cpp src/format.cpp src/http.cpp src/station_monitor.cpp src/packages.cpp src/northray.cpp)
target_include_directories(cjui PUBLIC include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)
target_compile_definitions(cjui PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(cjui PUBLIC cjcore d2d1 dwrite dwmapi windowscodecs ole32 winhttp bcrypt shell32 user32 gdi32 advapi32)
if(NOT MSVC)
  target_compile_options(cjui PRIVATE -Wall -Wextra -Wno-missing-field-initializers -Wno-cast-function-type)
endif()
