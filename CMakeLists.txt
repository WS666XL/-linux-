cmake_minimum_required(VERSION 3.16)
project(DistMonitor CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Protobuf REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GRPCPP REQUIRED grpc++)
pkg_check_modules(LIBBPF REQUIRED libbpf)
find_package(Threads REQUIRED)
find_path(MYSQLCLIENT_INCLUDE_DIR mysql.h PATHS /usr/include/mysql /usr/local/include/mysql)
find_library(MYSQLCLIENT_LIB mysqlclient REQUIRED)
find_program(GRPC_CPP_PLUGIN_EXE grpc_cpp_plugin REQUIRED)

# ---------- 生成 monitor.proto 的 protobuf/gRPC 代码 ----------
set(PROTO_GEN_DIR ${CMAKE_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})
set(PROTO_SRC ${PROTO_GEN_DIR}/monitor.pb.cc)
set(PROTO_HDR ${PROTO_GEN_DIR}/monitor.pb.h)
set(GRPC_SRC ${PROTO_GEN_DIR}/monitor.grpc.pb.cc)
set(GRPC_HDR ${PROTO_GEN_DIR}/monitor.grpc.pb.h)
add_custom_command(
  OUTPUT ${PROTO_SRC} ${PROTO_HDR} ${GRPC_SRC} ${GRPC_HDR}
  COMMAND protoc
          --cpp_out=${PROTO_GEN_DIR}
          --grpc_out=${PROTO_GEN_DIR}
          --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXE}
          -I${CMAKE_SOURCE_DIR}/proto
          ${CMAKE_SOURCE_DIR}/proto/monitor.proto
  DEPENDS ${CMAKE_SOURCE_DIR}/proto/monitor.proto
  COMMENT "Generating protobuf/gRPC code from monitor.proto"
  VERBATIM)

add_library(monitor_proto STATIC ${PROTO_SRC} ${GRPC_SRC})
target_include_directories(monitor_proto PUBLIC ${PROTO_GEN_DIR} ${GRPCPP_INCLUDE_DIRS})
target_link_directories(monitor_proto PUBLIC ${GRPCPP_LIBRARY_DIRS})
target_link_libraries(monitor_proto PUBLIC protobuf::libprotobuf ${GRPCPP_LIBRARIES})

# ---------- common ----------
add_library(common STATIC common/src/config.cpp)
target_include_directories(common PUBLIC common/include)
target_link_libraries(common PUBLIC Threads::Threads)

# ---------- 单元测试 ----------
enable_testing()
add_executable(unit_tests
  tests/test_main.cpp
  tests/test_config.cpp
  tests/test_alert.cpp
  tests/test_scheduler.cpp
  tests/test_blocking_queue.cpp
  manager/alert/alert_engine.cpp
  manager/scheduler/weighted_scheduler.cpp)
target_link_libraries(unit_tests PRIVATE common monitor_proto)
target_include_directories(unit_tests PRIVATE manager)
add_test(NAME unit_tests COMMAND unit_tests)

# ---------- 编译 eBPF 程序（CO-RE，独立于内核源码树） ----------
# 需要 clang；容器内构建可用 -DENABLE_EBPF_OBJ=OFF 跳过（运行时回退 /proc 采集）
option(ENABLE_EBPF_OBJ "Compile eBPF object with clang" ON)
set(BPF_SRC ${CMAKE_SOURCE_DIR}/agent/bpf/net_monitor.bpf.c)
set(BPF_OBJ ${CMAKE_BINARY_DIR}/bpf/net_monitor.bpf.o)
set(TC_BPF_SRC ${CMAKE_SOURCE_DIR}/agent/bpf/net_tc.bpf.c)
set(TC_BPF_OBJ ${CMAKE_BINARY_DIR}/bpf/net_tc.bpf.o)
if(ENABLE_EBPF_OBJ)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/bpf)
add_custom_command(
  OUTPUT ${BPF_OBJ}
  COMMAND clang -target bpf -g -O2 -c ${BPF_SRC} -o ${BPF_OBJ}
          -I${CMAKE_SOURCE_DIR}/agent/bpf
  DEPENDS ${BPF_SRC} ${CMAKE_SOURCE_DIR}/agent/bpf/vmlinux.h
  COMMENT "Building eBPF object net_monitor.bpf.o"
  VERBATIM)
add_custom_command(
  OUTPUT ${TC_BPF_OBJ}
  COMMAND clang -target bpf -g -O2 -c ${TC_BPF_SRC} -o ${TC_BPF_OBJ}
          -I${CMAKE_SOURCE_DIR}/agent/bpf
  DEPENDS ${TC_BPF_SRC} ${CMAKE_SOURCE_DIR}/agent/bpf/vmlinux.h
  COMMENT "Building eBPF TC object net_tc.bpf.o"
  VERBATIM)
add_custom_target(net_monitor_bpf ALL DEPENDS ${BPF_OBJ} ${TC_BPF_OBJ})
endif()

# ---------- agent ----------
add_executable(agent
  agent/main.cpp
  agent/host_info.cpp
  agent/collector/cpu_collector.cpp
  agent/collector/kmod_collector.cpp
  agent/collector/mem_collector.cpp
  agent/collector/load_collector.cpp
  agent/collector/disk_collector.cpp
  agent/collector/net_collector.cpp
  agent/bpf/ebpf_net_loader.cpp
  agent/bpf/ebpf_net_collector.cpp
  agent/bpf/tc_net_collector.cpp
  agent/reporter/grpc_reporter.cpp)
target_include_directories(agent PRIVATE ${LIBBPF_INCLUDE_DIRS})
target_link_directories(agent PRIVATE ${LIBBPF_LIBRARY_DIRS})
target_link_libraries(agent PRIVATE common monitor_proto ${LIBBPF_LIBRARIES})
if(ENABLE_EBPF_OBJ)
  add_dependencies(agent net_monitor_bpf)
endif()

# ---------- manager ----------
add_executable(manager
  manager/main.cpp
  manager/service/monitor_service.cpp
  manager/service/query_service.cpp
  manager/storage/mysql_storage.cpp
  manager/alert/alert_engine.cpp
  manager/scheduler/weighted_scheduler.cpp)
target_include_directories(manager PRIVATE manager ${MYSQLCLIENT_INCLUDE_DIR})
target_link_libraries(manager PRIVATE common monitor_proto ${MYSQLCLIENT_LIB})

# ---------- 查询客户端（QueryService 验证工具）----------
add_executable(query_client tools/query_client.cpp)
target_link_libraries(query_client PRIVATE common monitor_proto)

# ---------- 终端监控面板（无 Web 前端，直接在当前终端刷新）----------
add_executable(monitor_tui tools/monitor_tui.cpp)
target_link_libraries(monitor_tui PRIVATE monitor_proto)
