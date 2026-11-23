cmake_minimum_required(VERSION 3.20)

project(CoherencyLatencyMultiPlatform VERSION 1.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

if(APPLE)
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING
            "Minimum macOS version (Apple Silicon)" FORCE)
    endif()

    if(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING
            "Target architecture (Apple Silicon)" FORCE)
    endif()
endif()

find_package(Threads REQUIRED)

add_executable(coherency_latency src/main.cpp)
target_link_libraries(coherency_latency PRIVATE Threads::Threads)

if(MSVC)
    target_compile_options(coherency_latency PRIVATE
        /W4 /permissive-
        $<$<CONFIG:Release>:/O2>)
else()
    target_compile_options(coherency_latency PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<CONFIG:Release>:-O3>)
endif()

install(TARGETS coherency_latency RUNTIME DESTINATION bin)
