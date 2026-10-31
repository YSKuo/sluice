cmake_minimum_required(VERSION 3.25)
project(sluice LANGUAGES CXX)

# The event loop is built directly on epoll, signalfd, and accept4, which
# exist only on Linux. On macOS, build inside the devcontainer.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "sluice targets Linux only (epoll); use the devcontainer on other systems.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SLUICE_WERROR "Treat compiler warnings as errors" OFF)
option(SLUICE_BUILD_TESTS "Build the unit tests (downloads GoogleTest)" ON)
option(SLUICE_BUILD_FUZZERS "Build libFuzzer targets (needs clang)" OFF)

set(SLUICE_WARNING_FLAGS
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wnon-virtual-dtor
)

add_library(sluice_core
    src/version.cpp
    src/echo/echo_server.cpp
    src/http/headers.cpp
    src/http/http_server.cpp
    src/http/request.cpp
    src/http/request_parser.cpp
    src/http/response.cpp
    src/http/response_parser.cpp
    src/http/status.cpp
    src/http/syntax.cpp
    src/net/acceptor.cpp
    src/net/buffer.cpp
    src/net/channel.cpp
    src/net/connector.cpp
    src/net/event_loop.cpp
    src/net/inet_address.cpp
    src/net/socket.cpp
    src/net/tcp_connection.cpp
    src/net/tcp_server.cpp
    src/proxy/client_session.cpp
    src/proxy/forwarding.cpp
    src/proxy/proxy_server.cpp
    src/proxy/rate_limiter.cpp
    src/proxy/route_table.cpp
    src/proxy/upstream.cpp
    src/proxy/upstream_call.cpp
)
target_include_directories(sluice_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_options(sluice_core PUBLIC ${SLUICE_WARNING_FLAGS})
if(SLUICE_WERROR)
    target_compile_options(sluice_core PUBLIC -Werror)
endif()

add_executable(sluice src/main.cpp)
target_link_libraries(sluice PRIVATE sluice_core)

if(SLUICE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(SLUICE_BUILD_FUZZERS)
    # The fuzz preset compiles everything with -fsanitize=fuzzer-no-link
    # (coverage instrumentation); only the fuzzer itself links libFuzzer.
    add_executable(request_parser_fuzzer fuzz/request_parser_fuzzer.cpp)
    target_link_libraries(request_parser_fuzzer PRIVATE sluice_core)
    target_link_options(request_parser_fuzzer PRIVATE -fsanitize=fuzzer)
endif()
