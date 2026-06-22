find_package(benchmark CONFIG REQUIRED)

file(GLOB_RECURSE _benchmark_src *.cpp)

if (NOT _benchmark_src)
    return()
endif ()

add_executable(diniBenchmarks
    ${_benchmark_src}
)

target_compile_features(diniBenchmarks PRIVATE cxx_std_20)

target_link_libraries(diniBenchmarks PRIVATE
    dini::dini
    benchmark::benchmark
    benchmark::benchmark_main
)

add_test(
    NAME diniBenchmarks
    COMMAND $<TARGET_FILE:diniBenchmarks>
        --benchmark_min_time=0.005s
        --benchmark_repetitions=1
        --benchmark_report_aggregates_only=true
)

set_tests_properties(diniBenchmarks PROPERTIES
    ENVIRONMENT "PATH=$<TARGET_FILE_DIR:diniBenchmarks>;$ENV{PATH}"
)
