# WP-PAL: HAL — software IP tuner, demultiplexer and descrambler (HAL-TUN, HAL-DMX, HAL-DSC).
# The HAL calls no PAL module (rule R1): log lines go up through a callback (hal_log.hpp).
add_library(hal STATIC
    demux.cpp      # HAL-DMX (REQ-15, REQ-19)
    descrambler.cpp  # HAL-DSC (REQ-50)
    ip_tuner.cpp   # HAL-TUN (REQ-13)
)
target_include_directories(hal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hal PUBLIC OpenSSL::Crypto Threads::Threads)

add_executable(hal_tests
    tests/demux_test.cpp   # TST-211 – TST-214
    tests/descrambler_test.cpp   # TST-215 – TST-217
    tests/tuner_test.cpp   # TST-201 – TST-206
)
target_link_libraries(hal_tests PRIVATE hal GTest::gtest_main)
# Content tests (TST-216, TST-217) read content/ of the repository.
target_compile_definitions(hal_tests PRIVATE M15_REPO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../..")
gtest_discover_tests(hal_tests)
