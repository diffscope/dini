find_package(GTest CONFIG REQUIRED)

file(GLOB_RECURSE _test_src *.cpp)

if (NOT _test_src)
    return()
endif ()

add_executable(diniTests
    ${_test_src}
)

target_compile_features(diniTests PRIVATE cxx_std_20)

target_link_libraries(diniTests PRIVATE
    dini::dini
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(diniTests)
