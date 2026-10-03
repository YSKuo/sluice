cmake_minimum_required(VERSION 3.25)
project(sluice LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SLUICE_WERROR "Treat compiler warnings as errors" OFF)

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
)
target_include_directories(sluice_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_options(sluice_core PUBLIC ${SLUICE_WARNING_FLAGS})
if(SLUICE_WERROR)
    target_compile_options(sluice_core PUBLIC -Werror)
endif()

add_executable(sluice src/main.cpp)
target_link_libraries(sluice PRIVATE sluice_core)

enable_testing()
add_subdirectory(tests)
