include(FetchContent)

set(SLUICE_GOOGLETEST_TAG v1.15.2)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG ${SLUICE_GOOGLETEST_TAG}
)

# Match the CRT GoogleTest itself recommends when built alongside the project.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(sluice_tests
    smoke_test.cpp
)
target_link_libraries(sluice_tests PRIVATE sluice_core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(sluice_tests)
