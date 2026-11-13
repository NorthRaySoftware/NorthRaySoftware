add_library(cjcore STATIC
  src/time.cpp
  src/radio.cpp
  src/registry.cpp
  src/station.cpp
  src/skillswap.cpp
  src/ical.cpp
)
target_include_directories(cjcore
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)
if(MSVC)
  target_compile_options(cjcore PRIVATE /W4 /utf-8)
else()
  target_compile_options(cjcore PRIVATE -Wall -Wextra)
endif()
