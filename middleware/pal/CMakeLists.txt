# WP-PAL: PAL — DTV SDK (ITuner, IDemux) and platform services of IF-INT-04 over the HAL and Mongoose.
add_library(pal STATIC
    platform_placeholder.cpp
)
target_include_directories(pal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pal PUBLIC pal_api hal mongoose Threads::Threads)

add_executable(pal_tests
    tests/placeholder_test.cpp
)
target_link_libraries(pal_tests PRIVATE pal GTest::gtest_main)
gtest_discover_tests(pal_tests)
