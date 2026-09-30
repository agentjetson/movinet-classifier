cmake_minimum_required(VERSION 3.20)
project(temporal_classifier VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)
find_package(Threads REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio)
message(STATUS "OpenCV ${OpenCV_VERSION}")

# ONNX Runtime
if(NOT DEFINED ONNXRUNTIME_ROOT AND DEFINED ENV{ONNXRUNTIME_ROOT})
  set(ONNXRUNTIME_ROOT $ENV{ONNXRUNTIME_ROOT})
endif()
if(ONNXRUNTIME_ROOT)
  set(ONNXRUNTIME_INCLUDE_DIRS ${ONNXRUNTIME_ROOT}/include)
  find_library(ONNXRUNTIME_LIB onnxruntime
    PATHS ${ONNXRUNTIME_ROOT}/lib ${ONNXRUNTIME_ROOT}/lib64 NO_DEFAULT_PATH)
else()
  find_path(ONNXRUNTIME_INCLUDE_DIRS onnxruntime_cxx_api.h QUIET)
  find_library(ONNXRUNTIME_LIB onnxruntime QUIET)
endif()
if(NOT ONNXRUNTIME_LIB)
  message(WARNING "ONNX Runtime not found — set ONNXRUNTIME_ROOT. Stubs will be used.")
else()
  message(STATUS "ONNX Runtime: ${ONNXRUNTIME_LIB}")
endif()

find_package(spdlog QUIET)
if(NOT spdlog_FOUND)
  FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.17.0 GIT_SHALLOW TRUE)
  set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
  set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(spdlog)
endif()

add_executable(temporal_classifier
  src/main.cpp
  src/common/env.hpp
  src/temporal/movinet.cpp
  src/temporal/movinet.hpp
)

target_include_directories(temporal_classifier PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${OpenCV_INCLUDE_DIRS}
)
if(ONNXRUNTIME_INCLUDE_DIRS)
  target_include_directories(temporal_classifier PRIVATE ${ONNXRUNTIME_INCLUDE_DIRS})
endif()

target_link_libraries(temporal_classifier PRIVATE
  ${OpenCV_LIBS}
  Threads::Threads
  spdlog::spdlog
)
if(ONNXRUNTIME_LIB)
  target_link_libraries(temporal_classifier PRIVATE ${ONNXRUNTIME_LIB})
  target_compile_definitions(temporal_classifier PRIVATE TEMPORAL_HAS_ORT=1)
endif()

install(TARGETS temporal_classifier RUNTIME DESTINATION bin)
install(DIRECTORY config/ DESTINATION share/temporal-classifier/config OPTIONAL)
install(DIRECTORY models/ DESTINATION share/temporal-classifier/models OPTIONAL)
