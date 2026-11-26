# WP-PAL: process P4, OTT MW REST server (SRV-MW).
# The REST API and the EPG generation are a library, so that server_tests can test them in-process.
add_library(srvmw STATIC
    mw_api.cpp
)
target_include_directories(srvmw PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(srvmw PUBLIC srvcommon)

add_executable(mw_server
    main.cpp
)
target_link_libraries(mw_server PRIVATE srvmw)
