cmake_minimum_required(VERSION 3.5.0)
project(clbench)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

include_directories(${NATIVERENDER_ROOT_PATH})

add_library(clbench SHARED cl_bench.cpp)

target_link_libraries(clbench PUBLIC
    libace_napi.z.so
    libhilog_ndk.z.so
    libqos.so
)
