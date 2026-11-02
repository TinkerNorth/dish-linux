# The input-path latency bench. Bench-only: nothing here reaches the app or a package. Its vigem
# mode is Windows-only, so this build has the synthetic mode alone.

# A DISH_LOCK_STATS build compiles every TrackedMutex with counters, so its bench is the lockstats
# twin and the plain build's is the one whose latencies are reported.
if(DISH_LOCK_STATS)
    set(DISH_INPUT_PATH_BENCH bench_input_path_lockstats)
else()
    set(DISH_INPUT_PATH_BENCH bench_input_path)
endif()

add_executable(${DISH_INPUT_PATH_BENCH} input_path_bench.cpp)
target_include_directories(${DISH_INPUT_PATH_BENCH} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(${DISH_INPUT_PATH_BENCH}
    PRIVATE
        dish_core
        dish_warnings
        dish_strict)

if(DISH_BUILD_TESTS)
    add_test(NAME ${DISH_INPUT_PATH_BENCH}_smoke
        COMMAND ${DISH_INPUT_PATH_BENCH} --smoke
            --json ${CMAKE_CURRENT_BINARY_DIR}/${DISH_INPUT_PATH_BENCH}_smoke.json)
    # The smoke run judges every producer's pacing, which a parallel test's load would skew.
    set_tests_properties(${DISH_INPUT_PATH_BENCH}_smoke PROPERTIES RUN_SERIAL TRUE)
endif()
