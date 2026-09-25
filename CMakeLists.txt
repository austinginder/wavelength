cmake_minimum_required(VERSION 3.20)
# macOS builds are universal by default: render workers run Intel-only plugins (Reaktor 6, older VST2s)
# under Rosetta, which needs an x86_64 slice of this executable. -DCMAKE_OSX_ARCHITECTURES=arm64 opts out.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND NOT CMAKE_TOOLCHAIN_FILE AND NOT CMAKE_OSX_ARCHITECTURES)
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures" FORCE)
endif()
project(wavelength VERSION 0.3.0 LANGUAGES C CXX)
if(APPLE)
  enable_language(OBJCXX)
endif()

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
# VST3 SDK (MIT since 3.8): only the hosting sources are compiled, so SOURCE_SUBDIR points
# at a folder without a CMakeLists.txt to keep the SDK's own targets out of the build
FetchContent_Declare(vst3sdk
  GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
  GIT_TAG v3.8.1_build_84
  GIT_SHALLOW TRUE
  GIT_SUBMODULES base pluginterfaces public.sdk
  SOURCE_SUBDIR wavelength-no-build)
# zstd (BSD): Serum 2 presets and states are zstd-compressed CBOR
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zstd
  URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR build/cmake)
# Signalsmith Stretch + Linear (MIT, header-only): pitch-preserving time stretch for audio clips
FetchContent_Declare(signalsmith_stretch
  GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
  GIT_TAG 57b93f4e9206a089a45387eaa39bdc9f310d3308
  SOURCE_SUBDIR wavelength-no-build)
FetchContent_Declare(signalsmith_linear
  GIT_REPOSITORY https://github.com/Signalsmith-Audio/linear.git
  GIT_TAG 0.3.1
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR wavelength-no-build)
FetchContent_MakeAvailable(clap json vst3sdk zstd signalsmith_stretch signalsmith_linear)
# zlib (zlib licence): from the system on macOS and Linux, built here where there is none (Windows)
find_package(ZLIB)
if(NOT ZLIB_FOUND)
  FetchContent_Declare(zlib
    URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(zlib)
  target_include_directories(zlibstatic INTERFACE ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
  add_library(ZLIB::ZLIB ALIAS zlibstatic)
endif()

set(SDK ${vst3sdk_SOURCE_DIR})
add_library(vst3hosting STATIC
  ${SDK}/base/source/baseiids.cpp
  ${SDK}/base/source/fbuffer.cpp
  ${SDK}/base/source/fdebug.cpp
  ${SDK}/base/source/fobject.cpp
  ${SDK}/base/source/fstreamer.cpp
  ${SDK}/base/source/fstring.cpp
  ${SDK}/base/source/timer.cpp
  ${SDK}/base/source/updatehandler.cpp
  ${SDK}/base/thread/source/fcondition.cpp
  ${SDK}/base/thread/source/flock.cpp
  ${SDK}/pluginterfaces/base/conststringtable.cpp
  ${SDK}/pluginterfaces/base/coreiids.cpp
  ${SDK}/pluginterfaces/base/funknown.cpp
  ${SDK}/pluginterfaces/base/ustring.cpp
  ${SDK}/public.sdk/source/common/commoniids.cpp
  ${SDK}/public.sdk/source/common/commonstringconvert.cpp
  ${SDK}/public.sdk/source/common/memorystream.cpp
  ${SDK}/public.sdk/source/vst/vstinitiids.cpp
  ${SDK}/public.sdk/source/vst/vstpresetfile.cpp
  ${SDK}/public.sdk/source/vst/hosting/connectionproxy.cpp
  ${SDK}/public.sdk/source/vst/hosting/eventlist.cpp
  ${SDK}/public.sdk/source/vst/hosting/hostclasses.cpp
  ${SDK}/public.sdk/source/vst/hosting/module.cpp
  ${SDK}/public.sdk/source/vst/hosting/parameterchanges.cpp
  ${SDK}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
  ${SDK}/public.sdk/source/vst/hosting/plugprovider.cpp
  ${SDK}/public.sdk/source/vst/hosting/processdata.cpp
  ${SDK}/public.sdk/source/vst/utility/stringconvert.cpp)
if(APPLE)
  target_sources(vst3hosting PRIVATE ${SDK}/public.sdk/source/vst/hosting/module_mac.mm ${SDK}/public.sdk/source/common/threadchecker_mac.mm)
  set_source_files_properties(${SDK}/public.sdk/source/vst/hosting/module_mac.mm ${SDK}/public.sdk/source/common/threadchecker_mac.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
  target_link_libraries(vst3hosting PUBLIC "-framework CoreFoundation" "-framework Foundation")
elseif(WIN32)
  target_sources(vst3hosting PRIVATE ${SDK}/public.sdk/source/vst/hosting/module_win32.cpp ${SDK}/public.sdk/source/common/threadchecker_win32.cpp)
  target_link_libraries(vst3hosting PUBLIC ole32 shell32)
elseif(UNIX)
  target_sources(vst3hosting PRIVATE ${SDK}/public.sdk/source/vst/hosting/module_linux.cpp ${SDK}/public.sdk/source/common/threadchecker_linux.cpp)
  target_link_libraries(vst3hosting PUBLIC dl pthread)
endif()
target_include_directories(vst3hosting SYSTEM PUBLIC ${SDK})
target_compile_definitions(vst3hosting PUBLIC RELEASE=1)
target_compile_options(vst3hosting PRIVATE -w)

add_executable(wavelength
  src/main.cpp
  src/bundle.cpp
  src/builtins.cpp
  src/sampler.cpp
  src/effects.cpp
  src/engine.cpp
  src/loudness.cpp
  src/plugin.cpp
  src/presets.cpp
  src/preset_formats.cpp
  src/preset_files.cpp
  src/microtonic.cpp
  src/analyze.cpp
  src/audition.cpp
  src/clips.cpp
  src/instance.cpp
  src/platform.cpp
  src/catalog.cpp
  src/clap_plugin.cpp
  src/job.cpp
  src/render.cpp
  src/state_file.cpp
  src/vst3_plugin.cpp
  src/vst2_plugin.cpp
  src/zip.cpp
  src/xml.cpp
  src/dawproject.cpp
  src/bitwig.cpp
  src/wav.cpp)
# "-dev" between releases; cleared when a release is cut
set(WAVELENGTH_VERSION_SUFFIX "-dev")
target_compile_definitions(wavelength PRIVATE WAVELENGTH_VERSION="${PROJECT_VERSION}${WAVELENGTH_VERSION_SUFFIX}")
target_link_libraries(wavelength PRIVATE clap nlohmann_json::nlohmann_json vst3hosting ZLIB::ZLIB libzstd_static)
target_include_directories(wavelength SYSTEM PRIVATE ${zstd_SOURCE_DIR}/lib ${signalsmith_stretch_SOURCE_DIR}/include
                           ${signalsmith_linear_SOURCE_DIR}/include)
target_compile_options(wavelength PRIVATE -Wall -Wextra -Wno-unused-parameter)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(wavelength PRIVATE -Wno-psabi)   # GCC 10 ABI notes on arm64, not problems
endif()
if(APPLE)
  target_link_libraries(wavelength PRIVATE "-framework CoreFoundation" "-framework CoreGraphics")
elseif(WIN32)
  # UTF-8 as the process code page (file names), and a self-contained .exe
  target_sources(wavelength PRIVATE src/wavelength.rc)
  target_link_options(wavelength PRIVATE -static)
  target_compile_definitions(wavelength PRIVATE UNICODE _UNICODE)
elseif(UNIX)
  # runs on any distribution with glibc as new as the build machine's, without a matching libstdc++
  target_link_options(wavelength PRIVATE -static-libstdc++ -static-libgcc)
endif()
