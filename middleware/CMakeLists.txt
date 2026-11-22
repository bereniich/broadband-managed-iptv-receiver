# M15 Broadband Managed IPTV receiver — top-level CMake project (shared file, changed only on main).
# Builds the receiver (middleware/) and the operator side (server/) into build/bin/:
#   cmake -S middleware -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
#   ctest --test-dir build --output-on-failure
cmake_minimum_required(VERSION 3.28)
project(m15_iptv LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra)   # NFR-11: no warnings allowed

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

enable_testing()
find_package(GTest REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
include(GoogleTest)

set(M15_THIRD_PARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)

# IF-INT-04 headers (middleware/include/pal/platform.hpp): #include "pal/platform.hpp".
add_library(pal_api INTERFACE)
target_include_directories(pal_api INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# nlohmann/json 3.12.0, header only: #include <json.hpp>. Only its own directory is put on
# the include path, so that a user of JSON does not see the other third-party sources.
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE ${M15_THIRD_PARTY_DIR}/nlohmann)

# Mongoose 7.23 (HTTP client, HTTP / WebSocket server), compiled once as a static library.
add_library(mongoose STATIC ${M15_THIRD_PARTY_DIR}/mongoose/mongoose.c)
target_include_directories(mongoose PUBLIC ${M15_THIRD_PARTY_DIR}/mongoose)

# Receiver (process P2): AL (core, main), PAL (pal), HAL (hal).
add_subdirectory(core)
add_subdirectory(hal)
add_subdirectory(pal)
add_subdirectory(main)

# Operator side (processes P3, P4 and the packaging tool, Section 4.7).
add_subdirectory(../server server)
