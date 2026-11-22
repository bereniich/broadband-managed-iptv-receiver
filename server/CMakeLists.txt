# Operator side (Section 4.7), built by the CMake project of middleware/ into build/bin/.
# Rule R5: no target of the operator side links dtvcore, pal or hal.
# One CMakeLists.txt per directory, so that each work package changes only its own files
# (server/package/: WP-APP; all other directories: WP-PAL).
add_subdirectory(common)
add_subdirectory(headend)
add_subdirectory(mw)
add_subdirectory(package)
add_subdirectory(tests)
