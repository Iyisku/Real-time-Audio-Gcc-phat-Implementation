cmake_minimum_required(VERSION 3.14)
project(speeqr_server)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2 -pthread -g")

# Find required packages
find_package(Boost REQUIRED COMPONENTS system thread)
find_package(Threads REQUIRED)

# Download nlohmann/json
include(FetchContent)
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.2
)
FetchContent_MakeAvailable(json)

# Download KissFFT - Disable all extra components
include(FetchContent)
FetchContent_Declare(
    kissfft
    GIT_REPOSITORY https://github.com/mborgerding/kissfft.git
    GIT_TAG master
)

# Set variables BEFORE populating to disable tools and tests
set(KISSFFT_TOOLS OFF CACHE BOOL "Disable KissFFT tools" FORCE)
set(KISSFFT_TEST OFF CACHE BOOL "Disable KissFFT tests" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "Disable testing" FORCE)

FetchContent_MakeAvailable(kissfft)

# Override the tools directory to not build (alternative method)
if(TARGET tools)
    set_target_properties(tools PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()
if(TARGET test)
    set_target_properties(test PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

# Include directories
include_directories(
    ${Boost_INCLUDE_DIRS} 
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${kissfft_SOURCE_DIR}
)

# Source files
set(SOURCES
    src/main.cpp
    src/ntp_server.cpp
    src/http_handler.cpp
    src/websocket_handler.cpp
    src/audio_buffer.cpp
    src/gcc_phat.cpp
)

# Create executable
add_executable(speeqr_server ${SOURCES})

# Link libraries
target_link_libraries(speeqr_server
    ${Boost_LIBRARIES}
    Threads::Threads
    nlohmann_json::nlohmann_json
    kissfft
)