# WP-PAL: process P3, head-end (SRV-HEC, SRV-ARC, SRV-RTP).
# The modules are a library, so that server_tests can test them in-process.
add_library(srvheadend STATIC
    archive.cpp      # SRV-ARC (Section 4.7.4)
    head_end.cpp     # SRV-HEC (Section 4.7.6)
    rtp_sender.cpp   # SRV-RTP (Section 4.7.5)
)
target_include_directories(srvheadend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(srvheadend PUBLIC srvcommon)

add_executable(headend
    main.cpp
)
target_link_libraries(headend PRIVATE srvheadend)
