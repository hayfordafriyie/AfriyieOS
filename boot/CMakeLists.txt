# =============================================================================
# AfriyieOS — boot bridge build
#
# PC: produces BOOTX64.EFI, the UEFI application that firmware runs from
#     EFI/BOOT/BOOTX64.EFI on the EFI System Partition.
# =============================================================================

if(AF_TARGET STREQUAL "x86_64")

    # -------------------------------------------------------------------------
    # Substitute the kernel binary path into the embedding assembly source.
    #
    # configure_file rather than a -D flag: passing a quoted absolute path
    # through CMake, the compiler driver and NASM is a quoting minefield, and
    # one wrong backslash on Windows silently produces a bogus incbin path.
    # Forward slashes are accepted by NASM on every host.
    # -------------------------------------------------------------------------
    file(TO_CMAKE_PATH "${CMAKE_BINARY_DIR}/kernel.bin" AF_KERNEL_BIN_PATH)

    set(AF_KERNEL_BIN "${AF_KERNEL_BIN_PATH}")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/uefi/embed.asm.in"
        "${CMAKE_CURRENT_BINARY_DIR}/embed.asm"
        @ONLY)

    add_executable(afriyieos_boot
        uefi/efi_main.c
        "${CMAKE_CURRENT_BINARY_DIR}/embed.asm"
    )

    # The embedded kernel must exist before this link.
    add_dependencies(afriyieos_boot afriyieos_kernel)

    set_target_properties(afriyieos_boot PROPERTIES
        OUTPUT_NAME "BOOTX64"
        SUFFIX ".efi"
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
    )

    target_link_libraries(afriyieos_boot PRIVATE af_boot_flags)
    target_include_directories(afriyieos_boot PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/uefi")

    # -------------------------------------------------------------------------
    # PE/COFF link settings.
    #
    #   -e efi_main         entry point by name (the linker script's ENTRY is a
    #                       hint; the PE header needs the symbol)
    #   --subsystem 10      EFI application
    #   --image-base 0      let firmware choose, then relocate
    #   -nostdlib           no host runtime
    #   -Wl,--no-seh        the UEFI loader does not expect SEH tables
    # -------------------------------------------------------------------------
    target_link_options(afriyieos_boot PRIVATE
        -T "${CMAKE_CURRENT_SOURCE_DIR}/uefi/bootx64.lds"
        -nostdlib
        -static
        -Wl,-e,efi_main
        -Wl,--subsystem,10
        -Wl,--image-base,0
        -Wl,--no-seh
        -Wl,--build-id=none
        -Wl,-Map=${CMAKE_BINARY_DIR}/boot.map
    )

    message(STATUS "  boot bridge   : UEFI application (BOOTX64.EFI)")

else()
    message(STATUS "  boot bridge   : skipped for ${AF_TARGET} "
                   "(ARM64 uses U-Boot; see kernel/arch/arm64/README.md)")
endif()
