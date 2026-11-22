# WP-AL: AL-MAIN, entry point of the DTV middleware process P2 (Section 4.2.5).
add_executable(dtv-middleware
    main.cpp
)
target_link_libraries(dtv-middleware PRIVATE dtvcore pal hal)
