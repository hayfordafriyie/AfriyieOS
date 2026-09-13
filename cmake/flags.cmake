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
# Higher-half kernel
#
# OFF for v0.1. The final design (blueprint section 3.3) puts the kernel at
# 0xFFFFFFFF80000000, which requires paging — and v0.1 has no paging at all.
# Turning this on without also adding the v0.2 page-table bootstrap, the
# higher-half linker script and the physical load address produces a kernel that
# faults on its first instruction fetch.
#
# When v0.2 lands, this becomes the default and -mcmodel=kernel is applied.
# -----------------------------------------------------------------------------
option(AF_KERNEL_HIGHER_HALF
       "Link the kernel into the top 2 GiB (requires the v0.2 paging bootstrap)"
       OFF)

# -----------------------------------------------------------------------------
# Kernel flags (C11, privileged)
# -----------------------------------------------------------------------------
set(AF_KERNEL_C_FLAGS ${AF_COMMON_FLAGS}
    -std=gnu11
    -mno-red-zone           # x86_64 ONLY (see note below) - interrupt handlers clobber it
    -mno-sse -mno-sse2 -mno-mmx -mno-80387 -mno-avx
                            # no floating point in the kernel, ever
    -fno-asynchronous-unwind-tables
    -fno-unwind-tables
    # Mark the stack non-executable in every object. Without this, objects that
    # carry no .note.GNU-stack section make the linker warn that it is
    # "assuming an executable stack" — and an executable stack is a real
    # hardening loss, not a cosmetic warning.
    -Wa,--noexecstack
)

if(AF_KERNEL_HIGHER_HALF)
    # The default small code model cannot address anything above 2 GiB, so every
    # absolute reference into the higher half would be silently truncated.
    list(APPEND AF_KERNEL_C_FLAGS -mcmodel=kernel)
    message(STATUS "  kernel model  : higher half (-mcmodel=kernel)")
else()
    message(STATUS "  kernel model  : identity-mapped at 1 MiB (v0.1; "
                   "higher half arrives in v0.2)")
endif()

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
#
# The compile OPTIONS are wrapped in a $<COMPILE_LANGUAGE:C> generator
# expression. Without it CMake hands the same flags to every compiler in the
# target, including NASM, which then fails with
#
#     nasm: fatal: unrecognised output format `freestanding'
#
# NASM has its own option syntax and needs none of these GCC flags: it gets the
# object format from CMAKE_ASM_NASM_OBJECT_FORMAT (-f elf64), and -I/-D from
# target_include_directories/target_compile_definitions below are already valid
# NASM option spellings.
# -----------------------------------------------------------------------------
add_library(af_kernel_flags INTERFACE)
target_compile_options(af_kernel_flags INTERFACE
    $<$<COMPILE_LANGUAGE:C>:${AF_KERNEL_C_FLAGS}>)
target_include_directories(af_kernel_flags INTERFACE
    "${CMAKE_SOURCE_DIR}/kernel/include")
target_compile_definitions(af_kernel_flags INTERFACE
    AF_TARGET_X86_64=$<STREQUAL:${AF_TARGET},x86_64>
    AF_TARGET_AARCH64=$<STREQUAL:${AF_TARGET},aarch64>)

add_library(af_boot_flags INTERFACE)
target_compile_options(af_boot_flags INTERFACE
    $<$<COMPILE_LANGUAGE:C>:${AF_BOOT_FLAGS}>)

add_library(af_user_flags INTERFACE)
target_compile_options(af_user_flags INTERFACE
    $<$<COMPILE_LANGUAGE:C>:${AF_USER_C_FLAGS}>
    $<$<COMPILE_LANGUAGE:CXX>:${AF_USER_CXX_FLAGS}>)
target_include_directories(af_user_flags INTERFACE "${CMAKE_SOURCE_DIR}/libs/libaf/include")

message(STATUS "  kernel flags: ${AF_KERNEL_C_FLAGS}")
