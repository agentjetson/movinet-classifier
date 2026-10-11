cmake_minimum_required(VERSION 3.20)
project(movinet_classifier VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)
find_package(Threads REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio)
find_package(PkgConfig REQUIRED)

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

find_package(spdlog QUIET)
if(NOT spdlog_FOUND)
  FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.17.0 GIT_SHALLOW TRUE)
  set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
  set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(spdlog)
endif()

find_package(yaml-cpp QUIET)
if(NOT yaml-cpp_FOUND)
  FetchContent_Declare(yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG 0.8.0 GIT_SHALLOW TRUE)
  set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(yaml-cpp)
endif()

set(AJ_EDGE_COMMON_GIT_TAG main CACHE STRING "agentjetson/edge-common ref")
FetchContent_Declare(aj_edge_common
  GIT_REPOSITORY https://github.com/agentjetson/edge-common.git
  GIT_TAG ${AJ_EDGE_COMMON_GIT_TAG}
  GIT_SHALLOW TRUE)
set(AJ_EDGE_WITH_OTEL ON CACHE BOOL "" FORCE)
set(AJ_EDGE_WITH_NATS ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(aj_edge_common)

set(_use_grpc_config FALSE)
if(NOT APPLE)
  find_package(gRPC CONFIG QUIET)
  if(gRPC_FOUND)
    set(_use_grpc_config TRUE)
  endif()
endif()
if(_use_grpc_config)
  set(_GRPC_GRPCPP gRPC::grpc++)
  if(TARGET gRPC::grpc_cpp_plugin)
    set(_GRPC_CPP_PLUGIN $<TARGET_FILE:gRPC::grpc_cpp_plugin>)
  else()
    find_program(_GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)
  endif()
  if(NOT TARGET protobuf::libprotobuf)
    find_package(Protobuf REQUIRED)
  endif()
else()
  pkg_check_modules(PROTOBUF REQUIRED IMPORTED_TARGET protobuf)
  pkg_check_modules(GRPC REQUIRED IMPORTED_TARGET grpc++)
  if(NOT TARGET protobuf::libprotobuf)
    add_library(protobuf::libprotobuf ALIAS PkgConfig::PROTOBUF)
  endif()
  if(PROTOBUF_INCLUDE_DIRS)
    set(Protobuf_INCLUDE_DIRS ${PROTOBUF_INCLUDE_DIRS})
  endif()
  set(_GRPC_GRPCPP PkgConfig::GRPC)
  find_program(_GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)
  find_program(_PROTOBUF_PROTOC protoc REQUIRED)
endif()
if(NOT DEFINED _PROTOBUF_PROTOC)
  if(TARGET protobuf::protoc)
    set(_PROTOBUF_PROTOC $<TARGET_FILE:protobuf::protoc>)
  else()
    find_program(_PROTOBUF_PROTOC protoc REQUIRED)
  endif()
endif()

set(AGENTJETSON_CONTRACT_GIT_TAG main CACHE STRING "agentjetson/core ref")
FetchContent_Declare(agentjetson_contract
  GIT_REPOSITORY https://github.com/agentjetson/core.git
  GIT_TAG ${AGENTJETSON_CONTRACT_GIT_TAG}
  GIT_SHALLOW TRUE)
FetchContent_GetProperties(agentjetson_contract)
if(NOT agentjetson_contract_POPULATED)
  FetchContent_Populate(agentjetson_contract)
endif()
set(PROTO_ROOT ${agentjetson_contract_SOURCE_DIR}/proto)
set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

set(PROTO_FILES
  ${PROTO_ROOT}/common/v1/error.proto
  ${PROTO_ROOT}/capture/v1/frame.proto
  ${PROTO_ROOT}/capture/v1/frame_service.proto
  ${PROTO_ROOT}/scene/v1/scene.proto
  ${PROTO_ROOT}/detection/v1/detection.proto
  ${PROTO_ROOT}/ingest/v1/ingest_service.proto
)

set(_PROTO_INCLUDE_PATH "")
if(DEFINED Protobuf_INCLUDE_DIRS)
  foreach(_inc ${Protobuf_INCLUDE_DIRS})
    if(EXISTS "${_inc}/google/protobuf/descriptor.proto")
      set(_PROTO_INCLUDE_PATH "${_inc}")
      break()
    endif()
  endforeach()
endif()

set(PROTO_SRCS)
set(GRPC_SRCS)
foreach(proto ${PROTO_FILES})
  get_filename_component(proto_name ${proto} NAME_WE)
  get_filename_component(proto_dir ${proto} DIRECTORY)
  file(RELATIVE_PATH rel_dir ${PROTO_ROOT} ${proto_dir})
  set(pb_cc   ${PROTO_GEN_DIR}/${rel_dir}/${proto_name}.pb.cc)
  set(grpc_cc ${PROTO_GEN_DIR}/${rel_dir}/${proto_name}.grpc.pb.cc)
  set(_PROTOC_CMD ${_PROTOBUF_PROTOC} --proto_path=${PROTO_ROOT}
    --cpp_out=${PROTO_GEN_DIR} --grpc_out=${PROTO_GEN_DIR}
    --plugin=protoc-gen-grpc=${_GRPC_CPP_PLUGIN} ${proto})
  if(_PROTO_INCLUDE_PATH)
    list(INSERT _PROTOC_CMD 2 --proto_path=${_PROTO_INCLUDE_PATH})
  endif()
  add_custom_command(
    OUTPUT ${pb_cc} ${PROTO_GEN_DIR}/${rel_dir}/${proto_name}.pb.h
           ${grpc_cc} ${PROTO_GEN_DIR}/${rel_dir}/${proto_name}.grpc.pb.h
    COMMAND ${_PROTOC_CMD} DEPENDS ${proto}
    COMMENT "Generating ${rel_dir}/${proto_name}" VERBATIM)
  list(APPEND PROTO_SRCS ${pb_cc})
  list(APPEND GRPC_SRCS ${grpc_cc})
endforeach()

add_library(temporal_proto ${PROTO_SRCS} ${GRPC_SRCS})
target_include_directories(temporal_proto PUBLIC ${PROTO_GEN_DIR} ${Protobuf_INCLUDE_DIRS})
target_link_libraries(temporal_proto PUBLIC protobuf::libprotobuf ${_GRPC_GRPCPP})

add_executable(temporal_classifier
  src/main.cpp src/temporal/movinet.cpp src/temporal/taxonomy.cpp)
target_include_directories(temporal_classifier PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src ${OpenCV_INCLUDE_DIRS} ${PROTO_GEN_DIR})
if(ONNXRUNTIME_INCLUDE_DIRS)
  target_include_directories(temporal_classifier PRIVATE ${ONNXRUNTIME_INCLUDE_DIRS})
endif()
target_link_libraries(temporal_classifier PRIVATE
  temporal_proto aj::edge_common ${OpenCV_LIBS} Threads::Threads spdlog::spdlog yaml-cpp)
if(ONNXRUNTIME_LIB)
  target_link_libraries(temporal_classifier PRIVATE ${ONNXRUNTIME_LIB})
  target_compile_definitions(temporal_classifier PRIVATE TEMPORAL_HAS_ORT=1)
endif()

install(TARGETS temporal_classifier RUNTIME DESTINATION bin)
install(DIRECTORY config/ DESTINATION share/temporal-classifier/config OPTIONAL)

message(STATUS "movinet-classifier ${PROJECT_VERSION} edge-common=${AJ_EDGE_COMMON_GIT_TAG}")
