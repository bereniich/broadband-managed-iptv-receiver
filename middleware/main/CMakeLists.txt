# WP-AL: AL-MAIN, entry point of the DTV middleware process P2 (Section 4.2.5).
add_executable(dtv-middleware
    main.cpp
)
target_link_libraries(dtv-middleware PRIVATE dtvcore pal hal)

# TST-158 at process level: exit code 1 and a message naming the file (IN-11, ER-37).
add_test(NAME Tst158.MainExitCode
         COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/main_exit_code_test.sh $<TARGET_FILE:dtv-middleware>)
