cmake_minimum_required(VERSION 3.20)
project(PpmCodecExample LANGUAGES CXX)

set(FAR_RIDE_VIEWER_ROOT "" CACHE PATH
    "Path to the Far Ride Viewer source tree")

if(NOT EXISTS "${FAR_RIDE_VIEWER_ROOT}/include/ride_codec_api.h")
  message(FATAL_ERROR
    "Set FAR_RIDE_VIEWER_ROOT to the Far Ride Viewer source directory")
endif()

if(NOT MINGW)
  message(FATAL_ERROR
    "This example is intended for the MSYS2 UCRT64 MinGW toolchain")
endif()

add_library(ppm_codec SHARED ppm_codec.cpp)
target_include_directories(ppm_codec PRIVATE
  "${FAR_RIDE_VIEWER_ROOT}/include"
)
target_compile_features(ppm_codec PRIVATE cxx_std_17)
target_compile_definitions(ppm_codec PRIVATE
  RIDE_CODEC_BUILD
  WIN32_LEAN_AND_MEAN
  NOMINMAX
)
target_compile_options(ppm_codec PRIVATE -Wall -Wextra -Wpedantic)
target_link_options(ppm_codec PRIVATE
  -static
  -static-libgcc
  -static-libstdc++
)
set_target_properties(ppm_codec PROPERTIES
  CXX_EXTENSIONS OFF
  PREFIX ""
  OUTPUT_NAME "ppm"
  SUFFIX ".dll"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Codecs"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Codecs"
)

include(CTest)
if(BUILD_TESTING)
  add_executable(ppm_codec_test ppm_codec_test.cpp)
  target_include_directories(ppm_codec_test PRIVATE
    "${FAR_RIDE_VIEWER_ROOT}/include"
  )
  target_compile_features(ppm_codec_test PRIVATE cxx_std_17)
  target_link_libraries(ppm_codec_test PRIVATE ppm_codec)
  set_target_properties(ppm_codec_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Codecs"
  )
  add_test(NAME ppm_codec_test COMMAND ppm_codec_test)
endif()
