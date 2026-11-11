cmake_minimum_required(VERSION 3.20)
project(FarRideViewer LANGUAGES CXX)

set(FAR_SOURCE_DIR "" CACHE PATH "Path to Far Manager source tree containing far/plugin.hpp")
if(NOT EXISTS "${FAR_SOURCE_DIR}/far/plugin.hpp")
  message(FATAL_ERROR "Set FAR_SOURCE_DIR to Far Manager source tree")
endif()
if(NOT MINGW)
  message(FATAL_ERROR "This project is configured for MSYS2 UCRT64 MinGW")
endif()

find_package(Threads REQUIRED)

add_library(qoi_codec SHARED
  codecs/qoi/qoi_codec.cpp
  codecs/qoi/qoi_legacy.cpp
  codecs/qoi/qoi_legacy.h
)
target_include_directories(qoi_codec PRIVATE include codecs/qoi)
target_compile_features(qoi_codec PRIVATE cxx_std_17)
target_compile_definitions(qoi_codec PRIVATE RIDE_CODEC_BUILD WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(qoi_codec PRIVATE Threads::Threads)
set_target_properties(qoi_codec PROPERTIES
  CXX_EXTENSIONS OFF
  PREFIX ""
  OUTPUT_NAME "qoi"
  SUFFIX ".dll"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Codecs"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Codecs"
)

add_library(pcx_codec SHARED
  codecs/pcx/pcx_codec.cpp
)
target_include_directories(pcx_codec PRIVATE include)
target_compile_features(pcx_codec PRIVATE cxx_std_17)
target_compile_definitions(pcx_codec PRIVATE RIDE_CODEC_BUILD WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(pcx_codec PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(pcx_codec PROPERTIES
  CXX_EXTENSIONS OFF
  PREFIX ""
  OUTPUT_NAME "pcx"
  SUFFIX ".dll"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Codecs"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Codecs"
)

add_library(stb_image_codec SHARED
  codecs/stb_image/stb_image_codec.cpp
  third_party/stb/stb_image.h
)
target_include_directories(stb_image_codec PRIVATE include)
target_include_directories(stb_image_codec SYSTEM PRIVATE third_party/stb)
target_compile_features(stb_image_codec PRIVATE cxx_std_17)
target_compile_definitions(stb_image_codec PRIVATE RIDE_CODEC_BUILD WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(stb_image_codec PROPERTIES
  CXX_EXTENSIONS OFF
  PREFIX ""
  OUTPUT_NAME "stb_image"
  SUFFIX ".dll"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Codecs"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Codecs"
)

add_library(FarRideViewer SHARED
  src/plugin.cpp
  src/FarRideViewer.def
)
target_include_directories(FarRideViewer PRIVATE
  "${FAR_SOURCE_DIR}/far"
  include
  src
)
target_compile_features(FarRideViewer PRIVATE cxx_std_17)
target_compile_definitions(FarRideViewer PRIVATE
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
)
target_compile_options(FarRideViewer PRIVATE -Wall -Wextra -Wpedantic)
target_link_options(FarRideViewer PRIVATE -static -static-libgcc -static-libstdc++)
target_link_libraries(FarRideViewer PRIVATE
  user32
  gdi32
  msimg32
  Threads::Threads
)
set_target_properties(FarRideViewer PROPERTIES
  CXX_EXTENSIONS OFF
  PREFIX ""
  OUTPUT_NAME "FarRideViewer"
  SUFFIX ".dll"
)

if(MINGW)
  target_link_options(qoi_codec PRIVATE -static -static-libgcc -static-libstdc++)
  target_link_options(pcx_codec PRIVATE -static -static-libgcc -static-libstdc++)
  target_link_options(stb_image_codec PRIVATE -static -static-libgcc -static-libstdc++)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(qoi_codec_test tests/qoi_codec_test.cpp)
  target_include_directories(qoi_codec_test PRIVATE include codecs/qoi)
  target_compile_features(qoi_codec_test PRIVATE cxx_std_17)
  target_link_libraries(qoi_codec_test PRIVATE qoi_codec)
  set_target_properties(qoi_codec_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Codecs"
  )
  add_test(NAME qoi_codec_test COMMAND qoi_codec_test)

  add_executable(pcx_codec_test tests/pcx_codec_test.cpp)
  target_include_directories(pcx_codec_test PRIVATE include)
  target_compile_features(pcx_codec_test PRIVATE cxx_std_17)
  target_link_libraries(pcx_codec_test PRIVATE pcx_codec)
  set_target_properties(pcx_codec_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Codecs"
  )
  add_test(NAME pcx_codec_test COMMAND pcx_codec_test)
  set_tests_properties(pcx_codec_test PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tests"
  )

  add_executable(stb_image_codec_test tests/stb_image_codec_test.cpp)
  target_include_directories(stb_image_codec_test PRIVATE include)
  target_compile_features(stb_image_codec_test PRIVATE cxx_std_17)
  target_link_libraries(stb_image_codec_test PRIVATE stb_image_codec)
  set_target_properties(stb_image_codec_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Codecs"
  )
  add_test(NAME stb_image_codec_test COMMAND stb_image_codec_test)
endif()
