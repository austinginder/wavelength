cmake_minimum_required(VERSION 3.20)
project(wavelength VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
# CLAP: the plugin ABI (header-only, MIT)
FetchContent_Declare(clap
  GIT_REPOSITORY https://github.com/free-audio/clap.git
  GIT_TAG 1.2.10
  GIT_SHALLOW TRUE)
# JSON for jobs and reports (header-only, MIT)
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(clap json)

add_executable(wavelength
  src/main.cpp
  src/bundle.cpp
  src/builtins.cpp
  src/effects.cpp
  src/engine.cpp
  src/loudness.cpp
  src/instance.cpp
  src/catalog.cpp
  src/job.cpp
  src/render.cpp
  src/state_file.cpp
  src/wav.cpp)
# "-dev" between releases; cleared when a release is cut
set(WAVELENGTH_VERSION_SUFFIX "-dev")
target_compile_definitions(wavelength PRIVATE WAVELENGTH_VERSION="${PROJECT_VERSION}${WAVELENGTH_VERSION_SUFFIX}")
target_link_libraries(wavelength PRIVATE clap nlohmann_json::nlohmann_json)
target_compile_options(wavelength PRIVATE -Wall -Wextra -Wno-unused-parameter)
if(APPLE)
  target_link_libraries(wavelength PRIVATE "-framework CoreFoundation")
endif()
