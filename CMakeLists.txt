cmake_minimum_required(VERSION 3.15)
project(SqlFromScratch)

# Use C++20 (or change to 23 for C++23)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_definitions(-w)

# Add more modern compiler flags
if(CMAKE_BUILD_TYPE MATCHES Debug)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -w")
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG -w")
endif()

file(GLOB_RECURSE SOURCES "src/*.cpp")
add_executable(${PROJECT_NAME} ${SOURCES})
