# Unit tests of the operator side (TST-3xx): WP-PAL, and WP-APP for SRV-PKG (TST-304 – TST-306).
add_executable(server_tests
    placeholder_test.cpp
)
target_link_libraries(server_tests PRIVATE srvcommon GTest::gtest_main)
gtest_discover_tests(server_tests)
