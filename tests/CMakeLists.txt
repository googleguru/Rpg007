find_package(GTest QUIET)
if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/v1.14.0.tar.gz)
    FetchContent_MakeAvailable(googletest)
endif()

add_executable(rba_tests
    test_ga_net_ordering.cpp
    test_aco_path_search.cpp
    test_pso_cost_tuner.cpp
    test_abc_via_minimizer.cpp
)
target_link_libraries(rba_tests PRIVATE rba_lib GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(rba_tests)
