cmake_minimum_required(VERSION 3.20)

project(CoherencyLatencyLauncher VERSION 1.0.0 LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "The CoherencyLatency launcher is Windows-only.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(PAYLOAD_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../windows")
set(PAYLOAD_X86 "${PAYLOAD_ROOT}/x86/CoherencyLatency.exe")
set(PAYLOAD_X64 "${PAYLOAD_ROOT}/x64/CoherencyLatency.exe")
set(PAYLOAD_ARM64 "${PAYLOAD_ROOT}/ARM64/CoherencyLatency.exe")

foreach(payload IN ITEMS "${PAYLOAD_X86}" "${PAYLOAD_X64}" "${PAYLOAD_ARM64}")
    if(NOT EXISTS "${payload}")
        message(FATAL_ERROR
            "Missing ${payload}. Build all three payloads with windows/src/build.bat first.")
    endif()
endforeach()

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/launcher.rc.in"
               "${CMAKE_CURRENT_BINARY_DIR}/launcher.rc" @ONLY)

add_executable(CoherencyLatency launcher.cpp "${CMAKE_CURRENT_BINARY_DIR}/launcher.rc")

target_compile_definitions(CoherencyLatency PRIVATE
    UNICODE _UNICODE _WIN32_WINNT=0x0600)
target_compile_options(CoherencyLatency PRIVATE /W4 /O2)
target_link_options(CoherencyLatency PRIVATE /SUBSYSTEM:CONSOLE,6.00)

foreach(config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${config}" CONFIG_UPPER)
    set_target_properties(CoherencyLatency PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER} "${CMAKE_CURRENT_BINARY_DIR}")
endforeach()
