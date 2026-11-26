# WP-PAL: HAL — software IP tuner, demultiplexer and descrambler (HAL-TUN, HAL-DMX, HAL-DSC).
# The HAL calls no PAL module (rule R1): log lines go up through a callback (hal_log.hpp).
add_library(hal STATIC
    demux.cpp      # HAL-DMX (REQ-15, REQ-19)
    ip_tuner.cpp   # HAL-TUN (REQ-13)
)
target_include_directories(hal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hal PUBLIC OpenSSL::Crypto Threads::Threads)

add_executable(hal_tests
    tests/demux_test.cpp   # TST-211 – TST-214
    tests/tuner_test.cpp   # TST-201 – TST-206
)
target_link_libraries(hal_tests PRIVATE hal GTest::gtest_main)
gtest_discover_tests(hal_tests)
