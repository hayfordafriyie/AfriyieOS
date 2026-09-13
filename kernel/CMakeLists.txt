# =============================================================================
# AfriyieOS — kernel build
#
# Produces the flat kernel image the boot bridge loads:
#   kernel.elf  — linked ELF, used by GDB and by the size budget check
#   kernel.bin  — flat binary, what actually goes on the ESP
# =============================================================================

set(AF_KERNEL_SOURCES
    # --- core: architecture independent ---
    core/kmain.c
    core/arch_hooks.c
    core/boot_info.c
    core/status.c
    core/log.c
    core/panic.c
    core/kstring.c
    core/fb.c
    core/font8x16.c
    core/splash.c
    core/selftest.c
)

# -----------------------------------------------------------------------------
# Per-architecture sources
# -----------------------------------------------------------------------------
if(AF_TARGET STREQUAL "x86_64")
    set(AF_ARCH_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/arch/x86_64")
    set(AF_LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/linker/x86_64.lds")

    set(AF_ARCH_C_SOURCES
        arch/x86_64/serial.c
        arch/x86_64/gdt.c
        arch/x86_64/idt.c
        arch/x86_64/arch.c
    )

    set(AF_ARCH_ASM_SOURCES
        arch/x86_64/entry.asm
        arch/x86_64/isr.asm
        arch/x86_64/cpu.asm
    )
elseif(AF_TARGET STREQUAL "aarch64")
    message(FATAL_ERROR
        "The ARM64 (phone) port is scheduled for milestone v1.1 — see "
        "docs/AfriyieOS-Blueprint.md section 11. The x86_64 kernel is being "
        "completed first, deliberately: ADR-007 in the blueprint. "
        "kernel/arch/arm64/ contains the port plan.")
endif()

# -----------------------------------------------------------------------------
# Assemble the kernel
#
# NASM is used for x86_64 assembly (blueprint section 4.1). It is enabled as a
# CMake language in the top-level project() so that the .asm files above are
# handled natively rather than being shoved through the C compiler.
# -----------------------------------------------------------------------------
add_executable(afriyieos_kernel
    ${AF_KERNEL_SOURCES}
    ${AF_ARCH_C_SOURCES}
    ${AF_ARCH_ASM_SOURCES}
)

set_target_properties(afriyieos_kernel PROPERTIES
    OUTPUT_NAME "kernel"
    LINK_DEPENDS "${AF_LINKER_SCRIPT}"
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

target_link_libraries(afriyieos_kernel PRIVATE af_kernel_flags)

target_link_options(afriyieos_kernel PRIVATE
    -T "${AF_LINKER_SCRIPT}"
    -nostdlib
    -static
    -Wl,--build-id=none          # a build id would land in a discarded section
    -Wl,--gc-sections
    -Wl,-Map=${CMAKE_BINARY_DIR}/kernel.map
    -lgcc                        # __udivdi3 etc. for 64-bit division on freestanding
)

# The flat binary the boot bridge loads.
add_custom_command(TARGET afriyieos_kernel POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary
            $<TARGET_FILE:afriyieos_kernel>
            ${CMAKE_BINARY_DIR}/kernel.bin
    COMMENT "Producing flat kernel binary"
    VERBATIM)

# -----------------------------------------------------------------------------
# Size budget (blueprint section 14): .text must stay under 64 KiB.
# This is a warning at v0.1, before the check is promoted to a CI failure.
# -----------------------------------------------------------------------------
add_custom_target(kernel-size
    COMMAND ${CMAKE_SIZE} -A $<TARGET_FILE:afriyieos_kernel>
    COMMAND ${CMAKE_SIZE} -A $<TARGET_FILE:afriyieos_kernel> | grep -E "^\\.text"
    COMMENT "Reporting kernel size against the 64 KiB .text budget"
    VERBATIM)

message(STATUS "  kernel arch   : ${AF_TARGET}")
message(STATUS "  linker script : ${AF_LINKER_SCRIPT}")
