# WP-APP: offline packaging tool SRV-PKG (Section 4.7.3). The D-04 code is a library, so that the unit
# tests TST-304 – TST-306 of server_tests can use it; it shares no code with the receiver (rule R5).
add_library(srvpackage STATIC
    packager.cpp   # PAT / PMT, CRC-32, AES-128-CBC per TS packet (D-04)
)
target_include_directories(srvpackage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(srvpackage PUBLIC OpenSSL::Crypto)

add_executable(package_ts
    main.cpp
)
target_link_libraries(package_ts PRIVATE srvpackage srvcommon)
