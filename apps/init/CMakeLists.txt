# =============================================================================
# AfriyieOS — init
#
# The first program that is genuinely a program: compiled by the cross
# compiler, linked at 4 GiB, written into the FAT32 volume as an ELF file, read
# back off the disk by the kernel's ELF loader, and entered in ring 3.
# =============================================================================

add_executable(afriyieos_init
    main.c
    $<TARGET_OBJECTS:af_crt0>)

set_target_properties(afriyieos_init PROPERTIES
    OUTPUT_NAME "init"
    SUFFIX ".elf"
    # The build root, alongside kernel.bin, because that is where mkimage.py
    # looks for the things it packs into the image.
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    LINK_DEPENDS "${AF_USER_LINKER_SCRIPT}"
    C_STANDARD 11
    C_STANDARD_REQUIRED ON)

target_link_libraries(afriyieos_init PRIVATE af af_user_flags)

target_link_options(afriyieos_init PRIVATE
    -T "${AF_USER_LINKER_SCRIPT}"
    -nostdlib
    -static
    -Wl,--build-id=none
    -lgcc)              # 64-bit division helpers, in case optimisation is off
