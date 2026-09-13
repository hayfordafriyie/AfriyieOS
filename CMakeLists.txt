# =============================================================================
# AfriyieOS — top-level build orchestration
#
# Two targets per run:
#   -DAF_TARGET=x86_64   (PC, UEFI)          toolchain: cmake/toolchain-x86_64-elf.cmake
#   -DAF_TARGET=aarch64  (Phone, U-Boot)     toolchain: cmake/toolchain-aarch64-elf.cmake
#
# Usage:
#   cmake -B build/x86_64 -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-x86_64-elf.cmake -DAF_TARGET=x86_64
#   cmake --build build/x86_64
# =============================================================================

cmake_minimum_required(VERSION 3.28)

project(AfriyieOS
    VERSION 0.1.0
    DESCRIPTION "AfriyieOS - a minimal microkernel OS for PCs and phones"
    LANGUAGES C CXX ASM)

# -----------------------------------------------------------------------------
# Target architecture selection
# -----------------------------------------------------------------------------
if(NOT DEFINED AF_TARGET)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
        set(AF_TARGET "x86_64" CACHE STRING "AfriyieOS target architecture")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        set(AF_TARGET "aarch64" CACHE STRING "AfriyieOS target architecture")
    else()
        message(FATAL_ERROR
            "Cannot infer AF_TARGET from CMAKE_SYSTEM_PROCESSOR='${CMAKE_SYSTEM_PROCESSOR}'. "
            "Pass -DAF_TARGET=x86_64 or -DAF_TARGET=aarch64 explicitly.")
    endif()
endif()

set_property(CACHE AF_TARGET PROPERTY STRINGS x86_64 aarch64)

if(NOT AF_TARGET MATCHES "^(x86_64|aarch64)$")
    message(FATAL_ERROR "AF_TARGET must be 'x86_64' or 'aarch64' (got '${AF_TARGET}')")
endif()

message(STATUS "AfriyieOS ${PROJECT_VERSION} — target: ${AF_TARGET}")
message(STATUS "  C compiler : ${CMAKE_C_COMPILER}")
message(STATUS "  C++ compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "  System proc : ${CMAKE_SYSTEM_PROCESSOR}")

# -----------------------------------------------------------------------------
# Options
# -----------------------------------------------------------------------------
option(AF_BUILD_BOOT       "Build the boot bridge (UEFI app / U-Boot payload)" ON)
option(AF_BUILD_KERNEL     "Build the microkernel"                            ON)
option(AF_BUILD_USERSPACE  "Build libs, servers, drivers and apps"            OFF)
option(AF_BUILD_TESTS      "Build in-kernel and host-side tests"              ON)
option(AF_DEBUG_BUILD      "Enable kernel debug assertions and redzones"      ON)
option(AF_WERROR           "Treat warnings as errors"                         ON)

if(AF_TARGET STREQUAL "aarch64" AND AF_BUILD_BOOT)
    message(STATUS "  boot bridge: ARM64 (u-boot) — PC UEFI stage skipped")
endif()

# -----------------------------------------------------------------------------
# Flags, toolchain helpers and packaging utilities
# -----------------------------------------------------------------------------
include(cmake/flags.cmake)

# -----------------------------------------------------------------------------
# Subdirectories
# -----------------------------------------------------------------------------
if(AF_BUILD_KERNEL)
    add_subdirectory(kernel)
endif()

if(AF_BUILD_BOOT)
    add_subdirectory(boot)
endif()

if(AF_BUILD_USERSPACE)
    add_subdirectory(libs)
    add_subdirectory(drivers)
    add_subdirectory(servers)
    add_subdirectory(apps)
endif()

if(AF_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/kernel/CMakeLists.txt")
    add_subdirectory(tests/kernel)
endif()

# -----------------------------------------------------------------------------
# Disk image packaging
# -----------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(AF_IMAGE_PATH "${CMAKE_BINARY_DIR}/afriyieos.img" CACHE FILEPATH
    "Path of the bootable disk image produced by the image target")

add_custom_target(image
    COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/tools/mkimage.py"
            --arch ${AF_TARGET}
            --build-dir "${CMAKE_BINARY_DIR}"
            --output "${AF_IMAGE_PATH}"
    DEPENDS afriyieos_kernel
    COMMENT "Packaging bootable AfriyieOS disk image for ${AF_TARGET}"
    VERBATIM)

add_custom_target(run
    COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/tools/run_qemu.py"
            --arch ${AF_TARGET}
            --image "${AF_IMAGE_PATH}"
    DEPENDS image
    COMMENT "Booting AfriyieOS in QEMU (${AF_TARGET})"
    USES_TERMINAL
    VERBATIM)

add_custom_target(debug
    COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/tools/run_qemu.py"
            --arch ${AF_TARGET}
            --image "${AF_IMAGE_PATH}"
            --debug
    DEPENDS image
    COMMENT "Booting AfriyieOS in QEMU with a GDB stub on :1234"
    USES_TERMINAL
    VERBATIM)

# -----------------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------------
add_custom_target(af-summary
    COMMAND ${CMAKE_COMMAND} -E echo "=============================================="
    COMMAND ${CMAKE_COMMAND} -E echo " AfriyieOS ${PROJECT_VERSION}"
    COMMAND ${CMAKE_COMMAND} -E echo " target          : ${AF_TARGET}"
    COMMAND ${CMAKE_COMMAND} -E echo " build type      : ${CMAKE_BUILD_TYPE}"
    COMMAND ${CMAKE_COMMAND} -E echo " boot bridge     : ${AF_BUILD_BOOT}"
    COMMAND ${CMAKE_COMMAND} -E echo " userspace       : ${AF_BUILD_USERSPACE}"
    COMMAND ${CMAKE_COMMAND} -E echo " debug build     : ${AF_DEBUG_BUILD}"
    COMMAND ${CMAKE_COMMAND} -E echo " output dir      : ${CMAKE_BINARY_DIR}"
    COMMAND ${CMAKE_COMMAND} -E echo "=============================================="
    COMMENT "AfriyieOS build configuration"
    VERBATIM)
