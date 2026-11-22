# WP-APP: offline packaging tool SRV-PKG (Section 4.7.3).
add_executable(package_ts
    main.cpp
)
target_link_libraries(package_ts PRIVATE srvcommon OpenSSL::Crypto)
