# WP-AL: DTV middleware library dtvcore (AL modules) and its unit tests (TST-1xx).
# Portability rule R3 / NFR-09: dtvcore uses only the C++17 standard library, nlohmann/json and
# the IF-INT-04 headers; dtvcore_tests links only dtvcore and GoogleTest (TST-159).
add_library(dtvcore STATIC
    placeholder.cpp
)
target_include_directories(dtvcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dtvcore PUBLIC pal_api nlohmann_json)

add_executable(dtvcore_tests
    tests/placeholder_test.cpp
)
target_include_directories(dtvcore_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(dtvcore_tests PRIVATE dtvcore GTest::gtest_main)
gtest_discover_tests(dtvcore_tests)
