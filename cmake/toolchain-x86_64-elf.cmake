# =============================================================================
# cmake/toolchain-x86_64-elf.cmake
#
# Cross-compilation for the bare-metal x86_64 PC target.
#
# Prerequisite: tools/build_toolchain.sh has produced x86_64-elf-gcc.
# Set CROSS_PREFIX to override the toolchain location.
# =============================================================================

set(CMAKE_SYSTEM_NAME       Generic)
set(CMAKE_SYSTEM_PROCESSOR  x86_64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(NOT DEFINED CROSS_PREFIX)
    if(DEFINED ENV{AF_CROSS_PREFIX})
        set(CROSS_PREFIX "$ENV{AF_CROSS_PREFIX}")
    elseif(DEFINED ENV{HOME})
        set(CROSS_PREFIX "$ENV{HOME}/opt/cross")
    else()
        set(CROSS_PREFIX "/opt/cross")
    endif()
endif()

set(AF_TRIPLE "x86_64-elf")

find_program(AF_C_COMPILER   NAMES ${AF_TRIPLE}-gcc
             HINTS "${CROSS_PREFIX}/bin" NO_DEFAULT_PATH)
find_program(AF_CXX_COMPILER NAMES ${AF_TRIPLE}-g++
             HINTS "${CROSS_PREFIX}/bin" NO_DEFAULT_PATH)
find_program(AF_LINKER       NAMES ${AF_TRIPLE}-ld
             HINTS "${CROSS_PREFIX}/bin" NO_DEFAULT_PATH)
find_program(AF_AS           NAMES ${AF_TRIPLE}-as
             HINTS "${CROSS_PREFIX}/bin" NO_DEFAULT_PATH)

if(NOT AF_C_COMPILER)
    message(FATAL_ERROR
        "x86_64-elf-gcc not found under '${CROSS_PREFIX}/bin'.\n"
        "Run ./tools/build_toolchain.sh first, or pass -DCROSS_PREFIX=/path/to/cross")
endif()

set(CMAKE_C_COMPILER   "${AF_C_COMPILER}")
set(CMAKE_CXX_COMPILER "${AF_CXX_COMPILER}" CACHE FILEPATH "" FORCE)
set(CMAKE_ASM_COMPILER "${AF_C_COMPILER}")
set(CMAKE_LINKER       "${AF_LINKER}")

# Bare metal: no host headers, no host libraries, no implicit executables.
set(CMAKE_FIND_ROOT_PATH "${CROSS_PREFIX}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CMAKE_C_COMPILER_WORKS   TRUE)
set(CMAKE_CXX_COMPILER_WORKS TRUE)

set(CMAKE_CROSSCOMPILING TRUE)
set(AF_TOOLCHAIN_TRIPLE "${AF_TRIPLE}")
