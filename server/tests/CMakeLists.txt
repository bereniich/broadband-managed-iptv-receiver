# Unit tests of the operator side (TST-3xx): WP-PAL, and WP-APP for SRV-PKG (TST-304 – TST-306).
add_executable(server_tests
    content_test.cpp   # TST-301 – TST-303 (SRV-GEN, WP-PAL)
    headend_test.cpp   # TST-311 – TST-317, TST-318 and TST-320 head-end parts (SRV-ARC, SRV-RTP, SRV-HEC; WP-PAL)
    mw_test.cpp        # TST-318 (OTT MW server part), TST-319, TST-320 (SRV-MW, configuration; WP-PAL)
    package_test.cpp   # TST-304 – TST-306 (SRV-PKG, WP-APP)
)
target_link_libraries(server_tests PRIVATE srvcommon srvheadend srvmw srvpackage OpenSSL::Crypto GTest::gtest_main)
# The content tests read content/ of the repository and write files for inspection into the build
# tree; TST-320 starts the server executables.
target_compile_definitions(server_tests PRIVATE
    M15_REPO_DIR="${CMAKE_SOURCE_DIR}/.."
    M15_TEST_OUTPUT_DIR="${CMAKE_BINARY_DIR}/test-output"
    M15_MW_SERVER_BIN="$<TARGET_FILE:mw_server>"
    M15_HEADEND_BIN="$<TARGET_FILE:headend>"
    M15_PACKAGE_TS_BIN="$<TARGET_FILE:package_ts>")
add_dependencies(server_tests mw_server headend package_ts)
# The content tests probe and decode 600 s files, TST-314 streams for 12 s; discovery needs no content.
gtest_discover_tests(server_tests PROPERTIES TIMEOUT 600)
