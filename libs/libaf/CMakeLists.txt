# =============================================================================
# AfriyieOS — libaf
#
# Three pieces, deliberately separate:
#
#   af.h    header-only. The system call stub and the string helpers. Inlined
#           into every caller, which is what makes a syscall cost one int 0x80.
#   af.c    the out-of-line parts. Only what assembly references by name.
#   crt0.S  the program entry point. Linked into each program as an OBJECT
#           library rather than into libaf itself, because _start must be
#           pulled in unconditionally — an archive member is only extracted
#           when it resolves an undefined symbol, and nothing calls _start, so
#           from a static library it would simply never be linked.
# =============================================================================

add_library(af STATIC af.c)
target_link_libraries(af PUBLIC af_user_flags)

add_library(af_crt0 OBJECT crt0.S)
target_link_libraries(af_crt0 PUBLIC af_user_flags)
set_target_properties(af_crt0 PROPERTIES POSITION_INDEPENDENT_CODE OFF)

# The linker script every user program links against. Exported through the
# cache because apps/ is a sibling directory and PARENT_SCOPE only reaches one
# level up.
set(AF_USER_LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/user.lds"
    CACHE INTERNAL "Linker script for user-space programs")
