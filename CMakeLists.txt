cmake_minimum_required(VERSION 3.21)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" CJ_VERSION)
string(STRIP "${CJ_VERSION}" CJ_VERSION)
project(CJCommunity VERSION ${CJ_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CJ_BUILD_TESTS "Build unit tests" ON)

# The software's development identity, from its one central setting.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shared/metadata/brand/identity.json" CJ_IDENTITY_JSON)
string(JSON CJ_DEVELOPER GET "${CJ_IDENTITY_JSON}" developer)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS shared/metadata/brand/identity.json)
add_compile_definitions("CJ_DEVELOPER_WSTR=L\"${CJ_DEVELOPER}\"" "CJ_DEVELOPER_STR=\"${CJ_DEVELOPER}\"")

# Portable core: registry, schedule, time rules. No Windows dependencies.
add_subdirectory(libs/cjcore)
add_subdirectory(libs/studiocore)

if(WIN32)
  add_subdirectory(libs/cjui)
  add_subdirectory(launcher)
  add_subdirectory(apps/cj-radio)
  add_subdirectory(apps/studio)
  add_subdirectory(apps/community)
  option(CJ_BUILD_SKILLSWAP "Build the postponed SkillSwap app" OFF)
  if(CJ_BUILD_SKILLSWAP)
    add_subdirectory(apps/skillswap)
  endif()
endif()

if(CJ_BUILD_TESTS)
  enable_testing()
endif()

# NorthRay Browser: its portable core and tests build everywhere, the app on Windows.
add_subdirectory(apps/browser)

if(CJ_BUILD_TESTS)
  add_executable(cjcore_tests tests/cjcore_tests.cpp)
  target_link_libraries(cjcore_tests PRIVATE cjcore)
  add_executable(studiocore_tests tests/studiocore_tests.cpp)
  target_link_libraries(studiocore_tests PRIVATE studiocore miniz)
  target_include_directories(studiocore_tests PRIVATE libs/third_party libs/studiocore/src libs/third_party/miniz)
  if(WIN32)
    target_link_options(studiocore_tests PRIVATE -static)
  endif()
  add_test(NAME studiocore COMMAND studiocore_tests)
  target_compile_definitions(cjcore_tests PRIVATE CJ_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  if(MINGW)
    target_link_options(cjcore_tests PRIVATE -static)
  endif()
  add_test(NAME cjcore_tests COMMAND cjcore_tests)
endif()

# Developer tool: shows what the Studio's importer finds in a file.
add_executable(studio_import_dump tests/import_dump.cpp)
target_link_libraries(studio_import_dump PRIVATE studiocore)
add_executable(studio_typeset_dump tests/typeset_dump.cpp)
target_link_libraries(studio_typeset_dump PRIVATE studiocore)
add_executable(studio_seed_demo tests/seed_demo.cpp)
target_link_libraries(studio_seed_demo PRIVATE studiocore)
target_include_directories(studio_seed_demo PRIVATE libs/third_party)
if(WIN32)
  target_link_options(studio_import_dump PRIVATE -static)
endif()
