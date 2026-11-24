# Unit tests of the operator side (TST-3xx): WP-PAL, and WP-APP for SRV-PKG (TST-304 – TST-306).
add_executable(server_tests
    content_test.cpp   # TST-301 – TST-303 (SRV-GEN, WP-PAL)
)
target_link_libraries(server_tests PRIVATE srvcommon GTest::gtest_main)
# The content tests read content/ of the repository and write files for inspection into the build tree.
target_compile_definitions(server_tests PRIVATE
    M15_REPO_DIR="${CMAKE_SOURCE_DIR}/.."
    M15_TEST_OUTPUT_DIR="${CMAKE_BINARY_DIR}/test-output")
# The content tests probe and decode 600 s files; discovery itself needs no content.
gtest_discover_tests(server_tests PROPERTIES TIMEOUT 600)
