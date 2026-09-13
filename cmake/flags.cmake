# =============================================================================
# cmake/flags.cmake — the canonical flag sets for AfriyieOS
#
# Everything that compiles for AfriyieOS links against one of these INTERFACE
# targets. GCC flags are NOT scattered across subdirectories; they live here so
# that the whole system can be audited in one place.
# =============================================================================

# -----------------------------------------------------------------------------
# Common, architecture-independent, freestanding flags
# -----------------------------------------------------------------------------
set(AF_COMMON_FLAGS
    -ffreestanding          # no hosted environment, no implicit libc assumptions
    -fno-stack-protector    # no __stack_chk_fail to link against
    -fno-pic -fno-pie       # no position-independent code: we link at fixed addresses
    -fno-omit-frame-pointer # keep frame pointers for the panic stack walker
    -fno-builtin            # do not assume standard library semantics
    -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-align -Wwrite-strings
    -Wredundant-decls -Wmissing-declarations -Wno-unused-parameter
)

if(AF_WERROR)
    list(APPEND AF_COMMON_FLAGS -Werror)
endif()

# -----------------------------------------------------------------------------
# Kernel flags (C11, privileged)
# -----------------------------------------------------------------------------
set(AF_KERNEL_C_FLAGS ${AF_COMMON_FLAGS}
    -std=gnu11
    -mcmodel=kernel         # kernel image lives in the top 2GB of the address space
    -mno-red-zone           # x86_64 ONLY (see note below) — interrupt handlers clobber it
    -mno-sse -mno-sse2 -mno-mmx -mno-80387 -mno-avx
                            # no floating point in the kernel, ever
    -fno-asynchronous-unwind-tables
    -fno-unwind-tables
)

# NOTE: -mno-red-zone, -mcmodel=kernel and the SSE/x87 family do not exist on
# ARM64. The arch-specific block below strips them and adds the ARM equivalents.
if(AF_TARGET STREQUAL "aarch64")
    list(REMOVE_ITEM AF_KERNEL_C_FLAGS
        -mno-red-zone -mcmodel=kernel -mno-sse -mno-sse2 -mno-mmx -mno-80387 -mno-avx)
    list(APPEND AF_KERNEL_C_FLAGS
        -mgeneral-regs-only     # forbid FP/SIMD register use in kernel code
        -mno-outline-atomics
        -march=armv8-a
    )
else()
    list(APPEND AF_KERNEL_C_FLAGS -mno-sahf -mno-80387)
endif()

# -----------------------------------------------------------------------------
# User-space flags (C++20, unprivileged)
#
# No exceptions, no RTTI, no thread-safe statics, no C++ runtime at all.
# libaf++ supplies the containers; see docs/AfriyieOS-Blueprint.md section 4.4.
# -----------------------------------------------------------------------------
set(AF_USER_CXX_FLAGS ${AF_COMMON_FLAGS}
    -std=c++20
    -fno-exceptions
    -fno-rtti
    -fno-threadsafe-statics
    -fno-use-cxa-atexit
    -nostdlib++ -nostdinc++
    -fno-operator-names
    -mno-red-zone
)

set(AF_USER_C_FLAGS ${AF_COMMON_FLAGS} -std=gnu11 -mno-red-zone)

if(AF_TARGET STREQUAL "aarch64")
    list(REMOVE_ITEM AF_USER_CXX_FLAGS -mno-red-zone)
    list(REMOVE_ITEM AF_USER_C_FLAGS   -mno-red-zone)
endif()

# -----------------------------------------------------------------------------
# Boot bridge flags
#
# The PC boot bridge is a PE/COFF UEFI application: it does NOT use the kernel
# memory model and it DOES need a relocatable output.
# -----------------------------------------------------------------------------
set(AF_BOOT_FLAGS
    -ffreestanding -fno-stack-protector -fshort-wchar
    -mno-red-zone -mno-sse -mno-sse2 -mno-mmx
    -Wall -Wextra -Wno-unused-parameter
)
if(AF_WERROR)
    list(APPEND AF_BOOT_FLAGS -Werror)
endif()

# -----------------------------------------------------------------------------
# Debug vs release
# -----------------------------------------------------------------------------
if(AF_DEBUG_BUILD)
    list(APPEND AF_KERNEL_C_FLAGS -O0 -g3 -DAF_DEBUG=1 -DAF_ASSERT_ENABLED=1)
else()
    list(APPEND AF_KERNEL_C_FLAGS -O2 -g -DNDEBUG -DAF_DEBUG=0 -DAF_ASSERT_ENABLED=0)
endif()

# -----------------------------------------------------------------------------
# Interface libraries — link these instead of repeating flags
# -----------------------------------------------------------------------------
add_library(af_kernel_flags INTERFACE)
target_compile_options(af_kernel_flags INTERFACE ${AF_KERNEL_C_FLAGS})
target_include_directories(af_kernel_flags INTERFACE
    "${CMAKE_SOURCE_DIR}/kernel/include")
target_compile_definitions(af_kernel_flags INTERFACE
    AF_TARGET_X86_64=$<STREQUAL:${AF_TARGET},x86_64>
    AF_TARGET_AARCH64=$<STREQUAL:${AF_TARGET},aarch64>)

add_library(af_boot_flags INTERFACE)
target_compile_options(af_boot_flags INTERFACE ${AF_BOOT_FLAGS})

add_library(af_user_flags INTERFACE)
target_compile_options(af_user_flags INTERFACE
    $<$<COMPILE_LANGUAGE:C>:${AF_USER_C_FLAGS}>
    $<$<COMPILE_LANGUAGE:CXX>:${AF_USER_CXX_FLAGS}>)
target_include_directories(af_user_flags INTERFACE "${CMAKE_SOURCE_DIR}/libs/libaf/include")

message(STATUS "  kernel flags: ${AF_KERNEL_C_FLAGS}")
