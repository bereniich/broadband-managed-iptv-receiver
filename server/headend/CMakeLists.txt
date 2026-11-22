# WP-PAL: process P3, head-end (SRV-HEC, SRV-ARC, SRV-RTP).
add_executable(headend
    main.cpp
)
target_link_libraries(headend PRIVATE srvcommon)
