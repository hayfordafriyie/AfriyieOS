# =============================================================================
# AfriyieOS — boot bridge build
#
# PC: produces boot/BOOTX64.EFI, the UEFI application firmware runs from
#     EFI/BOOT/BOOTX64.EFI on the EFI System Partition.
#
# =============================================================================
# WHY THIS TARGET USES A DIFFERENT COMPILER FROM THE KERNEL
# =============================================================================
# A UEFI application is a PE32+ executable. The kernel toolchain targets
# x86_64-elf, whose binutils has no PE emulation: `ld --subsystem` is simply not
# a recognised option, so the cross-compiler that builds the kernel cannot link
# the boot bridge at all.
#
# The two options are to rebuild binutils with PE targets enabled, or to use the
# PE toolchain that already exists. Rebuilding means every contributor and every
# CI cache key changes for one file; using the packaged MinGW PE compiler is
# standard practice for UEFI development on Linux and costs one apt package.
#
#   boot bridge : x86_64-w64-mingw32-gcc  -> PE32+  (this file)
#   kernel      : x86_64-elf-gcc          -> ELF64  (kernel/CMakeLists.txt)
#
# The boot bridge is one C file and one assembly file, so it is built with
# explicit custom commands rather than a nested CMake project. The commands are
# visible, and the compile flags are the same ones tools/compile_check.sh uses.
# =============================================================================

if(NOT AF_TARGET STREQUAL "x86_64")
    message(STATUS "  boot bridge   : skipped for ${AF_TARGET} "
                   "(ARM64 uses U-Boot; see kernel/arch/arm64/README.md)")
    return()
endif()

# -----------------------------------------------------------------------------
# Locate the PE toolchain
# -----------------------------------------------------------------------------
find_program(AF_MINGW_CC NAMES x86_64-w64-mingw32-gcc)
find_program(AF_MINGW_LD NAMES x86_64-w64-mingw32-ld)
find_program(AF_NASM_BOOT NAMES nasm)

if(NOT AF_MINGW_CC)
    message(FATAL_ERROR
        "x86_64-w64-mingw32-gcc not found. The UEFI boot bridge is a PE32+\n"
        "executable and cannot be linked by the x86_64-elf toolchain, which has\n"
        "no PE emulation.\n\n"
        "Install the PE toolchain:\n"
        "    sudo apt install gcc-mingw-w64-x86-64 binutils-mingw-w64-x86-64\n")
endif()

if(NOT AF_NASM_BOOT)
    message(FATAL_ERROR "nasm not found: needed to assemble boot/uefi/embed.asm")
endif()

message(STATUS "  boot bridge   : UEFI PE32+ via ${AF_MINGW_CC}")

# -----------------------------------------------------------------------------
# Embed the kernel image
#
# configure_file rather than a -D flag: passing a quoted absolute path through
# CMake, the compiler driver and NASM is a quoting minefield, and one wrong
# backslash on Windows silently produces a bogus incbin path. Forward slashes
# are accepted by NASM on every host.
# -----------------------------------------------------------------------------
file(TO_CMAKE_PATH "${CMAKE_BINARY_DIR}/kernel.bin" AF_KERNEL_BIN_PATH)
set(AF_KERNEL_BIN "${AF_KERNEL_BIN_PATH}")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/uefi/embed.asm.in"
    "${CMAKE_CURRENT_BINARY_DIR}/embed.asm"
    @ONLY)

set(AF_BOOT_OUT_DIR "${CMAKE_BINARY_DIR}/boot")
set(AF_EFI_OUT     "${AF_BOOT_OUT_DIR}/BOOTX64.EFI")

# -----------------------------------------------------------------------------
# Compile flags
#
# -ffreestanding          no hosted environment
# -fshort-wchar           CHAR16 is 16-bit, as UEFI requires
# -mno-red-zone           firmware may take an interrupt and clobber the red zone
# -mno-sse/-mmx           the firmware environment does not guarantee SSE on entry
# -maccumulate-outgoing-args   the Microsoft ABI's 32-byte shadow space
# -fno-asynchronous-unwind-tables  keeps .pdata small; UEFI does not use it
# -----------------------------------------------------------------------------
set(AF_BOOT_CFLAGS
    -ffreestanding -fno-stack-protector -fshort-wchar
    -mno-red-zone -mno-sse -mno-sse2 -mno-mmx
    -maccumulate-outgoing-args
    -fno-asynchronous-unwind-tables -fno-unwind-tables
    -Wall -Wextra -Wno-unused-parameter -std=gnu11 -g -O2)

if(AF_WERROR)
    list(APPEND AF_BOOT_CFLAGS -Werror)
endif()

# -----------------------------------------------------------------------------
# Custom commands
# -----------------------------------------------------------------------------
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/embed.o"
    COMMAND ${AF_NASM_BOOT} -f win64 -o "${CMAKE_CURRENT_BINARY_DIR}/embed.o"
            "${CMAKE_CURRENT_BINARY_DIR}/embed.asm"
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/embed.asm" afriyieos_kernel
    COMMENT "Assembling the embedded kernel image (nasm -f win64)"
    VERBATIM)

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/efi_main.o"
    COMMAND ${AF_MINGW_CC} ${AF_BOOT_CFLAGS}
            "-I${CMAKE_SOURCE_DIR}/kernel/include"
            "-I${CMAKE_CURRENT_SOURCE_DIR}/uefi"
            -c "${CMAKE_CURRENT_SOURCE_DIR}/uefi/efi_main.c"
            -o "${CMAKE_CURRENT_BINARY_DIR}/efi_main.o"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/uefi/efi_main.c"
            "${CMAKE_SOURCE_DIR}/kernel/include/afriyie/boot_info.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/uefi/efi.h"
    COMMENT "Compiling the UEFI boot bridge (PE/COFF)"
    VERBATIM)

add_custom_command(
    OUTPUT "${AF_EFI_OUT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${AF_BOOT_OUT_DIR}"
    # Quote the WHOLE -Wl argument, never a path inside it. Writing
    # -Wl,-T,"${dir}" makes CMake pass the quote characters through literally,
    # and the compiler driver then hands the linker a filename that begins with
    # a double quote — which fails as "cannot open linker script file".
    COMMAND ${AF_MINGW_CC} -nostdlib
            "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/uefi/bootx64.lds"
            -Wl,--subsystem,10          # EFI application
            -Wl,-e,efi_main
            -Wl,--image-base,0          # let firmware place and relocate it
            -Wl,--no-seh
            -Wl,--build-id=none
            "-Wl,-Map,${AF_BOOT_OUT_DIR}/boot.map"
            -o "${AF_EFI_OUT}"
            "${CMAKE_CURRENT_BINARY_DIR}/efi_main.o"
            "${CMAKE_CURRENT_BINARY_DIR}/embed.o"
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/efi_main.o"
            "${CMAKE_CURRENT_BINARY_DIR}/embed.o"
            "${CMAKE_CURRENT_SOURCE_DIR}/uefi/bootx64.lds"
    COMMENT "Linking BOOTX64.EFI (PE32+ EFI application)"
    VERBATIM)

# -----------------------------------------------------------------------------
# Targets
# -----------------------------------------------------------------------------
add_custom_target(afriyieos_boot ALL
    DEPENDS "${AF_EFI_OUT}"
    COMMENT "Building the UEFI boot bridge")

# Report the boot bridge and confirm it really came out as an EFI application.
# A silent mistake here produces an image firmware refuses to boot with no
# diagnostic whatsoever, so it is checked at build time rather than discovered
# at boot time.
add_custom_target(boot-verify
    COMMAND ${CMAKE_COMMAND} -E echo "--- boot/BOOTX64.EFI ---"
    COMMAND sh -c "ls -l '${AF_EFI_OUT}' | awk '{print \"size: \" \\$5 \" bytes\"}'"
    COMMAND sh -c "file '${AF_EFI_OUT}' | grep -q 'EFI (application)' \
                   && echo 'file type: PE32+ EFI application  OK' \
                   || { echo 'ERROR: BOOTX64.EFI is not an EFI application'; exit 1; }"
    COMMAND sh -c "objdump -h '${AF_EFI_OUT}' | tail -1 | grep -q '\\.reloc' \
                   && echo 'sections: .reloc is last  OK' \
                   || { echo 'WARNING: .reloc is not the last section; some firmware will refuse to relocate the image'; exit 1; }"
    DEPENDS afriyieos_boot
    COMMENT "Verifying the boot bridge is a valid UEFI application"
    VERBATIM)
