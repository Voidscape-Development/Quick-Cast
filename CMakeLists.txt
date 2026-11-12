cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)
option(ENABLE_TESTS "Build unit tests and developer tools for the OBS-independent core" OFF)
option(QUICK_CAST_BUILD_PLUGIN "Build the OBS plugin module (disable to build only the core library)" ON)

include(compilerconfig)
include(defaults)
include(helpers)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/finders")

# Third-party dependencies (libdatachannel, nlohmann_json) and the OBS-independent core library.
add_subdirectory(deps)
add_subdirectory(src/core)

if(ENABLE_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(NOT QUICK_CAST_BUILD_PLUGIN)
  return()
endif()

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets Core)
find_package(QCFFmpeg REQUIRED)

target_link_libraries(
  ${CMAKE_PROJECT_NAME}
  PRIVATE quick-cast-core OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Widgets QCFFmpeg::avcodec QCFFmpeg::avutil
)

target_compile_options(
  ${CMAKE_PROJECT_NAME}
  PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
)

set_target_properties(
  ${CMAKE_PROJECT_NAME}
  PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(MSVC)
  target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE /wd4244 /wd4267)
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/media/audio-decoder.cpp
    src/media/audio-decoder.hpp
    src/media/video-decoder.cpp
    src/media/video-decoder.hpp
    src/obs/guest-player.cpp
    src/obs/guest-player.hpp
    src/obs/guest-source.cpp
    src/obs/guest-source.hpp
    src/obs/return-output.cpp
    src/obs/return-output.hpp
    src/obs/scene-integration.cpp
    src/obs/scene-integration.hpp
    src/obs/session-controller.cpp
    src/obs/session-controller.hpp
    src/obs/settings-store.cpp
    src/obs/settings-store.hpp
    src/ui/guest-row.cpp
    src/ui/guest-row.hpp
    src/ui/quick-cast-dock.cpp
    src/ui/quick-cast-dock.hpp
    src/ui/settings-dialog.cpp
    src/ui/settings-dialog.hpp
    src/ui/ui-helpers.hpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
