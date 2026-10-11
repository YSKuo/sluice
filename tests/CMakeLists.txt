include(FetchContent)

set(SLUICE_GOOGLETEST_TAG v1.15.2)

# SYSTEM marks GoogleTest's headers as system headers, so the project's
# strict warning flags (-Wconversion -Werror, ...) do not apply to them.
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG ${SLUICE_GOOGLETEST_TAG}
    SYSTEM
)

# Match the CRT GoogleTest itself recommends when built alongside the project.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(sluice_tests
    buffer_test.cpp
    echo_server_test.cpp
    event_loop_test.cpp
    inet_address_test.cpp
    result_test.cpp
    smoke_test.cpp
    unique_fd_test.cpp
)
target_link_libraries(sluice_tests PRIVATE sluice_core GTest::gtest_main)

include(GoogleTest)
# Each test gets a hard timeout so a bug that blocks the event loop fails
# the test instead of hanging the whole run.
gtest_discover_tests(sluice_tests PROPERTIES TIMEOUT 30)
