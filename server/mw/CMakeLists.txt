# WP-PAL: process P4, OTT MW REST server (SRV-MW).
add_executable(mw_server
    main.cpp
)
target_link_libraries(mw_server PRIVATE srvcommon)
