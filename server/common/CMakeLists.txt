# WP-PAL: common helpers of the operator-side programs (configuration, logging, HTTP and JSON helpers, clock).
add_library(srvcommon STATIC
    config.cpp     # server/config.json (Section 4.7.1)
    http.cpp       # HTTP server and JSON helpers over Mongoose and nlohmann/json
    log.cpp        # log lines of NFR-13
    services.cpp   # content/services.json (IF-INT-05, Section 3.5)
)
target_include_directories(srvcommon PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(srvcommon PUBLIC nlohmann_json mongoose Threads::Threads)
