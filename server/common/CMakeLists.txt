# WP-PAL: common helpers of the operator-side programs (configuration, logging, HTTP and JSON helpers, clock).
add_library(srvcommon STATIC
    placeholder.cpp
)
target_include_directories(srvcommon PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(srvcommon PUBLIC nlohmann_json mongoose Threads::Threads)
