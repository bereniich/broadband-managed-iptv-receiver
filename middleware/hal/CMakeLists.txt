# WP-PAL: HAL — software IP tuner, demultiplexer and descrambler (HAL-TUN, HAL-DMX, HAL-DSC).
add_library(hal STATIC
    placeholder.cpp
)
target_include_directories(hal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hal PUBLIC OpenSSL::Crypto Threads::Threads)

add_executable(hal_tests
    tests/placeholder_test.cpp
)
target_link_libraries(hal_tests PRIVATE hal GTest::gtest_main)
gtest_discover_tests(hal_tests)
