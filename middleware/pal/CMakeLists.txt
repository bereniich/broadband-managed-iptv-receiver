# WP-PAL: PAL — DTV SDK (ITuner, IDemux) and platform services of IF-INT-04 over the HAL and Mongoose.
add_library(pal STATIC
    pal_clock.cpp          # PAL-CLK (IClock)
    pal_event_loop.cpp     # event-loop thread, command queue, timers (Section 4.5.2)
    pal_hls_store.cpp      # PAL-FS (IHlsStore)
    pal_http_client.cpp    # PAL-HTTP (IHttpClient)
    pal_logger.cpp         # PAL-LOG (ILogger)
    pal_mongoose_log.cpp   # Mongoose log lines into the PAL log
    pal_platform.cpp       # Platform, create_platform
    pal_sdk.cpp            # PAL-SDK (ITuner, IDemux over HAL-TUN, HAL-DMX)
    pal_ui_server.cpp      # PAL-WS (IUiChannel, IF-INT-01 – IF-INT-03 serving)
)
target_include_directories(pal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pal PUBLIC pal_api hal mongoose Threads::Threads)

add_executable(pal_tests
    tests/clock_logger_test.cpp   # TST-225, TST-226 (event loop)
    tests/hls_store_test.cpp      # TST-224
    tests/http_client_test.cpp    # TST-222
    tests/platform_test.cpp       # TST-226 (platform)
    tests/sdk_test.cpp            # TST-221 (without the key part)
    tests/ui_server_test.cpp      # TST-223
)
# The TS / RTP test helper of the HAL tests (Section 5.3.1, "Test helpers").
target_include_directories(pal_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../hal/tests)
target_link_libraries(pal_tests PRIVATE pal GTest::gtest_main)
gtest_discover_tests(pal_tests)
